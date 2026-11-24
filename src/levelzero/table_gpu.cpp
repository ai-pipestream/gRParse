#include "levelzero/table_gpu.h"

#include <level_zero/ze_api.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <format>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "grparse_inference_model.h"
#include "levelzero/device.h"
#include "levelzero/onnx_initializers.h"
#include "levelzero/graph_runtime.h"

namespace grparse::levelzero {

// slanet_decoder.cl as SPIR-V, generated at build time (cmake/LevelZero.cmake).
extern const unsigned char kSlanetDecoderSpirv[];
extern const size_t kSlanetDecoderSpirvSize;

namespace {

constexpr int kHidden = 256;
constexpr int kGates = 3 * kHidden;
constexpr int kWihStride = 160;          // slanet_decoder.cl's WIS
constexpr uint32_t kGroupSize = 1024;    // slanet_decoder.cl's L
constexpr uint32_t kGroupsPerTable = 16;  // past 16 a lone table runs no faster
constexpr size_t kExchangeBytes = (kHidden + kGates + kPositions + kGates + 2 * kHidden) * sizeof(float);
constexpr size_t kScratchFloats = size_t(kPositions) * kHidden + size_t(kChannels) * kPositions;
constexpr uint32_t kCounterStride = 32;  // one 128-byte line per table's barrier counter
constexpr size_t kImageFloats = size_t(3) * kImageSide * kImageSide;
constexpr size_t kFeatureFloats = size_t(kPositions) * kChannels;

// Output-major copy of an [in][out] matrix, rows `stride` floats apart.
void append_transposed(const OnnxTensor& tensor, int in, int out, int stride, std::vector<float>* packed) {
  const size_t start = packed->size();
  packed->resize(start + size_t(stride) * out, 0.0F);
  for (int i = 0; i < in; ++i) {
    for (int o = 0; o < out; ++o) (*packed)[start + size_t(o) * stride + i] = tensor.values[size_t(i) * out + o];
  }
}

}  // namespace

std::vector<float> pack_decoder_weights(const std::filesystem::path& step_model,
                                        std::vector<size_t>* offsets) {
  const auto weights = read_onnx_initializers(step_model);
  const auto get = [&](const char* name, std::vector<int64_t> dims) -> const OnnxTensor& {
    const auto found = weights.find(name);
    if (found == weights.end() || found->second.dims != dims) {
      throw std::runtime_error(std::format("{}: initializer {} is missing or not the decoder's shape",
                                           step_model.string(), name));
    }
    return found->second;
  };
  std::vector<float> packed;
  const auto begin = [&] {
    packed.resize((packed.size() + 15) / 16 * 16, 0.0F);  // 64-byte aligned parts
    offsets->push_back(packed.size());
  };
  const auto vector = [&](const char* name, int size) {
    begin();
    const auto& tensor = get(name, {size});
    packed.insert(packed.end(), tensor.values.begin(), tensor.values.end());
  };
  const auto matrix = [&](const char* name, int in, int out, int stride) {
    begin();
    append_transposed(get(name, {in, out}), in, out, stride, &packed);
  };
  offsets->clear();
  matrix("i2h", kChannels, kHidden, kChannels);
  matrix("h2h_w", kHidden, kHidden, kHidden);
  vector("h2h_b", kHidden);
  begin();
  const auto& score = get("score", {kHidden, 1});
  packed.insert(packed.end(), score.values.begin(), score.values.end());
  matrix("w_ih", kChannels + kVocab, kGates, kWihStride);
  matrix("w_hh", kHidden, kGates, kHidden);
  vector("b_ih", kGates);
  vector("b_hh", kGates);
  matrix("s1_w", kHidden, kHidden, kHidden);
  vector("s1_b", kHidden);
  matrix("s2_w", kHidden, kVocab, kHidden);
  vector("s2_b", kVocab);
  matrix("l1_w", kHidden, kHidden, kHidden);
  vector("l1_b", kHidden);
  matrix("l2_w", kHidden, 8, kHidden);
  vector("l2_b", 8);
  packed.resize((packed.size() + 15) / 16 * 16, 0.0F);
  return packed;
}

class TableStructureGpu::Impl {
 public:
  Impl(const std::filesystem::path& models_dir, int gpu_index) : device_(Device::shared(gpu_index)) {
    ze_module_handle_t module = device_.module(kSlanetDecoderSpirv, kSlanetDecoderSpirvSize, "table decoder");
    decoder_ = device_.kernel(module, "slanet_decode", kGroupSize, 1, 1);
    ze_check(zeKernelSuggestMaxCooperativeGroupCount(decoder_, &max_groups_), "cooperative group count");
    if (max_groups_ == 0) throw std::runtime_error("the device runs no cooperative group of the decoder");
    max_batch_ = std::min<uint32_t>(max_groups_, 16);
    // The encoder is an ordinary vision graph: it runs on the graph runtime,
    // in float (its features steer the decoder's tokens).
    encoder_ = std::make_unique<graph::GraphModel>(device_, models_dir / "slanet_plus_encoder.onnx");

    std::vector<size_t> offsets;
    const std::vector<float> weights = pack_decoder_weights(models_dir / "slanet_plus_step.onnx", &offsets);
    auto* decoder_weights = static_cast<float*>(device_.allocate(weights.size() * sizeof(float)));
    device_.upload(decoder_weights, weights.data(), weights.size() * sizeof(float));
    features_ = static_cast<float*>(device_.allocate(max_batch_ * kFeatureFloats * sizeof(float)));
    void* scratch = device_.allocate(max_batch_ * kScratchFloats * sizeof(float));
    void* exchange = device_.allocate(max_batch_ * kExchangeBytes);
    counters_ = device_.allocate(max_batch_ * kCounterStride * sizeof(uint32_t));
    logits_ = device_.allocate(max_batch_ * kMaxSteps * kVocab * sizeof(float));
    location_ = device_.allocate(max_batch_ * kMaxSteps * 8 * sizeof(float));
    steps_ = device_.allocate(max_batch_ * sizeof(int32_t));
    images_.resize(max_batch_ * kImageFloats);
    features_host_ = static_cast<float*>(device_.allocate_host(max_batch_ * kFeatureFloats * sizeof(float)));
    out_logits_ = static_cast<float*>(device_.allocate_host(max_batch_ * kMaxSteps * kVocab * sizeof(float)));
    out_location_ = static_cast<float*>(device_.allocate_host(max_batch_ * kMaxSteps * 8 * sizeof(float)));
    out_steps_ = static_cast<int32_t*>(device_.allocate_host(max_batch_ * sizeof(int32_t)));
    const auto argument = [&](uint32_t index, const void* pointer) {
      ze_check(zeKernelSetArgumentValue(decoder_, index, sizeof(void*), &pointer), "decoder argument");
    };
    argument(0, features_);
    for (uint32_t part = 0; part < offsets.size(); ++part) argument(1 + part, decoder_weights + offsets[part]);
    argument(17, scratch);
    argument(18, exchange);
    argument(19, counters_);
    argument(20, logits_);
    argument(21, location_);
    argument(22, steps_);

    // A compute queue that runs cooperative launches.
    uint32_t group_count = 0;
    zeDeviceGetCommandQueueGroupProperties(device_.device(), &group_count, nullptr);
    std::vector<ze_command_queue_group_properties_t> groups(
        group_count, ze_desc<ze_command_queue_group_properties_t>(ZE_STRUCTURE_TYPE_COMMAND_QUEUE_GROUP_PROPERTIES));
    ze_check(zeDeviceGetCommandQueueGroupProperties(device_.device(), &group_count, groups.data()), "queue groups");
    ordinal_ = group_count;
    for (uint32_t index = 0; index < group_count; ++index) {
      const auto flags = groups[index].flags;
      if ((flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COMPUTE) &&
          (flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COOPERATIVE_KERNELS)) {
        ordinal_ = index;
        break;
      }
    }
    if (ordinal_ == group_count) throw std::runtime_error("the device has no queue for cooperative kernels");
    auto queue_desc = ze_desc<ze_command_queue_desc_t>(ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC);
    queue_desc.ordinal = ordinal_;
    queue_desc.mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
    ze_check(zeCommandQueueCreate(device_.context(), device_.device(), &queue_desc, &queue_), "command queue");
    auto fence_desc = ze_desc<ze_fence_desc_t>(ZE_STRUCTURE_TYPE_FENCE_DESC);
    ze_check(zeFenceCreate(queue_, &fence_desc, &fence_), "fence");
    lists_.resize(max_batch_ + 1, nullptr);
    // Every batch size is recorded and run once now, so no request pays for
    // a recording or the driver's first-run setup.
    for (uint32_t tables = 1; tables <= max_batch_; ++tables) lists_[tables] = record(tables);
    const std::vector<float> blank(kImageFloats, 0.0F);
    std::vector<Pending> warm(max_batch_, Pending{blank.data(), {}, {}, false});
    std::vector<Pending*> batch;
    for (Pending& item : warm) batch.push_back(&item);
    run(batch);
  }

  DecodedTable decode(std::span<const float> image) {
    if (image.size() != kImageFloats) throw std::invalid_argument("table structure: the image must be 3 x 488 x 488");
    Pending pending{image.data(), {}, {}, false};
    std::unique_lock lock(mutex_);
    queue_items_.push_back(&pending);
    while (!pending.done) {
      if (running_) {
        idle_.wait(lock);
        continue;
      }
      // Lead: everything queued so far goes out in this batch.
      running_ = true;
      std::vector<Pending*> batch;
      while (!queue_items_.empty() && batch.size() < max_batch_) {
        batch.push_back(queue_items_.front());
        queue_items_.pop_front();
      }
      lock.unlock();
      std::exception_ptr error;
      try {
        run(batch);
      } catch (...) {
        error = std::current_exception();
      }
      lock.lock();
      for (Pending* item : batch) {
        item->error = error;
        item->done = true;
      }
      running_ = false;
      idle_.notify_all();
    }
    if (pending.error) std::rethrow_exception(pending.error);
    return std::move(pending.result);
  }

  const std::string& description() const { return device_.description(); }

 private:
  struct Pending {
    const float* image;
    DecodedTable result;
    std::exception_ptr error;
    bool done = false;
  };

  // The decoder's half of a batch: the encoder's features up, the
  // cooperative launch, the readback, in order.
  ze_command_list_handle_t record(uint32_t tables) {
    auto desc = ze_desc<ze_command_list_desc_t>(ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC);
    desc.commandQueueGroupOrdinal = ordinal_;
    ze_command_list_handle_t list = nullptr;
    ze_check(zeCommandListCreate(device_.context(), device_.device(), &desc, &list), "command list");
    ze_check(zeCommandListAppendMemoryCopy(list, features_, features_host_, tables * kFeatureFloats * sizeof(float),
                                           nullptr, 0, nullptr),
             "feature upload");
    const uint32_t zero = 0;
    ze_check(zeCommandListAppendMemoryFill(list, counters_, &zero, sizeof(zero),
                                           size_t(tables) * kCounterStride * sizeof(uint32_t), nullptr, 0, nullptr),
             "barrier reset");
    ze_check(zeCommandListAppendBarrier(list, nullptr, 0, nullptr), "barrier");
    const uint32_t groups = std::clamp<uint32_t>(max_groups_ / tables, 1, kGroupsPerTable);
    ze_check(zeKernelSetArgumentValue(decoder_, 23, sizeof(groups), &groups), "decoder argument");
    const ze_group_count_t count{tables * groups, 1, 1};
    ze_check(zeCommandListAppendLaunchCooperativeKernel(list, decoder_, &count, nullptr, 0, nullptr), "decoder launch");
    ze_check(zeCommandListAppendBarrier(list, nullptr, 0, nullptr), "barrier");
    const auto readback = [&](void* host, const void* source, size_t bytes) {
      ze_check(zeCommandListAppendMemoryCopy(list, host, source, bytes, nullptr, 0, nullptr), "readback");
    };
    readback(out_logits_, logits_, tables * kMaxSteps * kVocab * sizeof(float));
    readback(out_location_, location_, tables * kMaxSteps * 8 * sizeof(float));
    readback(out_steps_, steps_, tables * sizeof(int32_t));
    ze_check(zeCommandListClose(list), "command list close");
    return list;
  }

  void device_step(ze_result_t result, const char* step) const {
    if (result != ZE_RESULT_SUCCESS) {
      throw InferenceDeviceError(std::format("table structure: {} {} failed ({})", device_.description(), step,
                                             ze_name(result)));
    }
  }

  void run(const std::vector<Pending*>& batch) {
    const auto tables = static_cast<uint32_t>(batch.size());
    for (uint32_t index = 0; index < tables; ++index) {
      std::memcpy(images_.data() + index * kImageFloats, batch[index]->image, kImageFloats * sizeof(float));
    }
    const std::vector<graph::HostTensor> encoded =
        encoder_->run(images_.data(), {int64_t(tables), 3, kImageSide, kImageSide});
    if (encoded.size() != 1 || encoded[0].count() != tables * kFeatureFloats) {
      throw std::runtime_error("table structure: the encoder's output is not [tables][256][96]");
    }
    std::memcpy(features_host_, encoded[0].floats(), encoded[0].count() * sizeof(float));
    device_step(zeCommandQueueExecuteCommandLists(queue_, 1, &lists_[tables], fence_), "submission");
    device_step(zeFenceHostSynchronize(fence_, UINT64_MAX), "run");
    device_step(zeFenceReset(fence_), "fence reset");
    for (uint32_t index = 0; index < tables; ++index) {
      DecodedTable& out = batch[index]->result;
      out.steps = out_steps_[index];
      if (out.steps < 1 || out.steps > kMaxSteps) {
        throw InferenceDeviceError(std::format("table structure: {} returned {} steps", device_.description(), out.steps));
      }
      const float* logits = out_logits_ + size_t(index) * kMaxSteps * kVocab;
      const float* location = out_location_ + size_t(index) * kMaxSteps * 8;
      out.logits.assign(logits, logits + size_t(out.steps) * kVocab);
      out.location.assign(location, location + size_t(out.steps) * 8);
    }
  }

  Device& device_;
  ze_kernel_handle_t decoder_ = nullptr;
  uint32_t max_groups_ = 0;
  uint32_t max_batch_ = 1;
  uint32_t ordinal_ = 0;
  std::unique_ptr<graph::GraphModel> encoder_;
  ze_command_queue_handle_t queue_ = nullptr;
  ze_fence_handle_t fence_ = nullptr;
  std::vector<ze_command_list_handle_t> lists_;
  float* features_ = nullptr;
  void* counters_ = nullptr;
  void* logits_ = nullptr;
  void* location_ = nullptr;
  void* steps_ = nullptr;
  std::vector<float> images_;       // a batch's images, back to back
  float* features_host_ = nullptr;  // host memory the feature upload reads
  float* out_logits_ = nullptr;    // host memory the readback writes
  float* out_location_ = nullptr;
  int32_t* out_steps_ = nullptr;

  std::mutex mutex_;
  std::condition_variable idle_;
  std::deque<Pending*> queue_items_;
  bool running_ = false;
};

TableStructureGpu::TableStructureGpu(const std::filesystem::path& models_dir, int gpu_index)
    : impl_(std::make_unique<Impl>(models_dir, gpu_index)) {}

TableStructureGpu::~TableStructureGpu() = default;

DecodedTable TableStructureGpu::decode(std::span<const float> image) { return impl_->decode(image); }

const std::string& TableStructureGpu::description() const { return impl_->description(); }

TableStructureGpu& TableStructureGpu::shared(const std::filesystem::path& models_dir, int gpu_index) {
  static std::mutex mutex;
  static std::map<std::pair<std::string, int>, std::unique_ptr<TableStructureGpu>> instances;
  std::lock_guard lock(mutex);
  auto& slot = instances[{models_dir.string(), gpu_index}];
  if (slot == nullptr) slot = std::make_unique<TableStructureGpu>(models_dir, gpu_index);
  return *slot;
}

}  // namespace grparse::levelzero
