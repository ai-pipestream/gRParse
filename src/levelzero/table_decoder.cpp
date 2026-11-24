#include "levelzero/table_decoder.h"

#include <level_zero/ze_api.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <format>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "grparse_inference_model.h"
#include "levelzero/onnx_initializers.h"

namespace grparse::levelzero {

// slanet_decoder.cl as SPIR-V, generated at build time (cmake/LevelZero.cmake).
extern const unsigned char kSlanetDecoderSpirv[];
extern const size_t kSlanetDecoderSpirvSize;

namespace {

constexpr int kHidden = 256;
constexpr int kGates = 3 * kHidden;
constexpr int kWihStride = 160;      // slanet_decoder.cl's WIS
constexpr uint32_t kGroupSize = 1024;  // slanet_decoder.cl's L
constexpr uint32_t kGroupsPerTable = 16;  // past 16 a lone table runs no faster
constexpr size_t kExchangeBytes = (kHidden + kGates + kPositions + kGates + 2 * kHidden) * sizeof(float);
constexpr size_t kScratchFloats = size_t(kPositions) * kHidden + size_t(kChannels) * kPositions;
constexpr uint32_t kCounterStride = 32;  // one 128-byte line per table's barrier counter

std::string ze_name(ze_result_t result) { return std::format("0x{:08x}", static_cast<uint32_t>(result)); }

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

class TableDecoder::Impl {
 public:
  Impl(const std::filesystem::path& step_model, int gpu_index) {
    const auto check = [&](ze_result_t result, const char* step) {
      if (result != ZE_RESULT_SUCCESS) {
        throw std::runtime_error(std::format("Level Zero {} failed ({})", step, ze_name(result)));
      }
    };
    check(zeInit(ZE_INIT_FLAG_GPU_ONLY), "initialization (is the Intel GPU driver installed?)");
    uint32_t driver_count = 0;
    check(zeDriverGet(&driver_count, nullptr), "driver count");
    std::vector<ze_driver_handle_t> drivers(driver_count);
    check(zeDriverGet(&driver_count, drivers.data()), "driver list");
    int seen = 0;
    for (ze_driver_handle_t candidate : drivers) {
      uint32_t count = 0;
      check(zeDeviceGet(candidate, &count, nullptr), "device count");
      std::vector<ze_device_handle_t> devices(count);
      check(zeDeviceGet(candidate, &count, devices.data()), "device list");
      for (ze_device_handle_t device : devices) {
        ze_device_properties_t properties{ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES};
        check(zeDeviceGetProperties(device, &properties), "device properties");
        if (properties.type != ZE_DEVICE_TYPE_GPU || seen++ != gpu_index) continue;
        driver_ = candidate;
        device_ = device;
        description_ = std::format("Level Zero GPU {} ({})", gpu_index, properties.name);
      }
    }
    if (device_ == nullptr) {
      throw std::runtime_error(std::format("Level Zero has no GPU {} ({} found)", gpu_index, seen));
    }
    ze_context_desc_t context_desc{ZE_STRUCTURE_TYPE_CONTEXT_DESC};
    check(zeContextCreate(driver_, &context_desc, &context_), "context creation");
    ze_module_desc_t module_desc{ZE_STRUCTURE_TYPE_MODULE_DESC};
    module_desc.format = ZE_MODULE_FORMAT_IL_SPIRV;
    module_desc.inputSize = kSlanetDecoderSpirvSize;
    module_desc.pInputModule = kSlanetDecoderSpirv;
    module_desc.pBuildFlags = "";
    ze_module_build_log_handle_t log = nullptr;
    const ze_result_t built = zeModuleCreate(context_, device_, &module_desc, &module_, &log);
    if (built != ZE_RESULT_SUCCESS) {
      size_t size = 0;
      zeModuleBuildLogGetString(log, &size, nullptr);
      std::string text(size, '\0');
      zeModuleBuildLogGetString(log, &size, text.data());
      zeModuleBuildLogDestroy(log);
      throw std::runtime_error(std::format("the decoder kernel did not build ({}): {}", ze_name(built), text));
    }
    if (log != nullptr) zeModuleBuildLogDestroy(log);
    ze_kernel_desc_t kernel_desc{ZE_STRUCTURE_TYPE_KERNEL_DESC};
    kernel_desc.pKernelName = "slanet_decode";
    check(zeKernelCreate(module_, &kernel_desc, &kernel_), "kernel creation");
    check(zeKernelSetGroupSize(kernel_, kGroupSize, 1, 1), "group size");
    check(zeKernelSuggestMaxCooperativeGroupCount(kernel_, &max_groups_), "cooperative group count");
    if (max_groups_ == 0) throw std::runtime_error("the device runs no cooperative group of the decoder");
    max_batch_ = std::min<uint32_t>(max_groups_, 16);
    ze_command_queue_desc_t queue_desc{ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC};
    queue_desc.mode = ZE_COMMAND_QUEUE_MODE_SYNCHRONOUS;
    check(zeCommandListCreateImmediate(context_, device_, &queue_desc, &list_), "command list");

    std::vector<size_t> offsets;
    const std::vector<float> weights = pack_decoder_weights(step_model, &offsets);
    weights_ = allocate(weights.size() * sizeof(float));
    check(zeCommandListAppendMemoryCopy(list_, weights_, weights.data(), weights.size() * sizeof(float),
                                        nullptr, 0, nullptr),
          "weight upload");
    features_ = allocate(size_t(max_batch_) * kPositions * kChannels * sizeof(float));
    scratch_ = allocate(size_t(max_batch_) * kScratchFloats * sizeof(float));
    exchange_ = allocate(size_t(max_batch_) * kExchangeBytes);
    counters_ = allocate(size_t(max_batch_) * kCounterStride * sizeof(uint32_t));
    logits_ = allocate(size_t(max_batch_) * kMaxSteps * kVocab * sizeof(float));
    location_ = allocate(size_t(max_batch_) * kMaxSteps * 8 * sizeof(float));
    steps_ = allocate(size_t(max_batch_) * sizeof(int32_t));
    const auto argument = [&](uint32_t index, const void* pointer) {
      check(zeKernelSetArgumentValue(kernel_, index, sizeof(void*), &pointer), "kernel argument");
    };
    argument(0, features_);
    for (uint32_t part = 0; part < offsets.size(); ++part) {
      argument(1 + part, static_cast<const float*>(weights_) + offsets[part]);
    }
    argument(17, scratch_);
    argument(18, exchange_);
    argument(19, counters_);
    argument(20, logits_);
    argument(21, location_);
    argument(22, steps_);
  }

  ~Impl() {
    for (void* memory : allocations_) zeMemFree(context_, memory);
    if (list_ != nullptr) zeCommandListDestroy(list_);
    if (kernel_ != nullptr) zeKernelDestroy(kernel_);
    if (module_ != nullptr) zeModuleDestroy(module_);
    if (context_ != nullptr) zeContextDestroy(context_);
  }

  DecodedTable decode(std::span<const float> features) {
    if (features.size() != size_t(kPositions) * kChannels) {
      throw std::invalid_argument("table decoder: features must be 256 x 96");
    }
    Pending pending{features.data()};
    std::unique_lock lock(mutex_);
    queue_.push_back(&pending);
    while (!pending.done) {
      if (launching_) {
        idle_.wait(lock);
        continue;
      }
      // Lead: everything queued so far goes out in this launch.
      launching_ = true;
      std::vector<Pending*> batch;
      while (!queue_.empty() && batch.size() < max_batch_) {
        batch.push_back(queue_.front());
        queue_.pop_front();
      }
      lock.unlock();
      std::exception_ptr error;
      try {
        launch(batch);
      } catch (...) {
        error = std::current_exception();
      }
      lock.lock();
      for (Pending* item : batch) {
        item->error = error;
        item->done = true;
      }
      launching_ = false;
      idle_.notify_all();
    }
    if (pending.error) std::rethrow_exception(pending.error);
    return std::move(pending.result);
  }

  const std::string& description() const { return description_; }

 private:
  struct Pending {
    const float* features;
    DecodedTable result;
    std::exception_ptr error;
    bool done = false;
  };

  void* allocate(size_t bytes) {
    ze_device_mem_alloc_desc_t desc{ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC};
    void* memory = nullptr;
    const ze_result_t result = zeMemAllocDevice(context_, &desc, bytes, 64, device_, &memory);
    if (result != ZE_RESULT_SUCCESS) {
      throw std::runtime_error(std::format("Level Zero device allocation of {} bytes failed ({})", bytes,
                                           ze_name(result)));
    }
    allocations_.push_back(memory);
    return memory;
  }

  void device_step(ze_result_t result, const char* step) const {
    if (result != ZE_RESULT_SUCCESS) {
      throw InferenceDeviceError(std::format("table structure decoder: {} {} failed ({})", description_,
                                             step, ze_name(result)));
    }
  }

  // One cooperative launch for the batch: kGroupsPerTable groups per table,
  // fewer when the batch would not fit the device's resident groups.
  void launch(const std::vector<Pending*>& batch) {
    const auto tables = static_cast<uint32_t>(batch.size());
    const uint32_t groups = std::clamp<uint32_t>(max_groups_ / tables, 1, kGroupsPerTable);
    constexpr size_t kFeatureBytes = size_t(kPositions) * kChannels * sizeof(float);
    for (uint32_t index = 0; index < tables; ++index) {
      device_step(zeCommandListAppendMemoryCopy(list_, static_cast<char*>(features_) + index * kFeatureBytes,
                                                batch[index]->features, kFeatureBytes, nullptr, 0, nullptr),
                  "feature upload");
    }
    const uint32_t zero = 0;
    device_step(zeCommandListAppendMemoryFill(list_, counters_, &zero, sizeof(zero),
                                              size_t(tables) * kCounterStride * sizeof(uint32_t), nullptr, 0,
                                              nullptr),
                "barrier reset");
    device_step(zeKernelSetArgumentValue(kernel_, 23, sizeof(groups), &groups), "kernel argument");
    const ze_group_count_t count{tables * groups, 1, 1};
    device_step(zeCommandListAppendLaunchCooperativeKernel(list_, kernel_, &count, nullptr, 0, nullptr),
                "launch");
    std::vector<int32_t> steps(tables);
    device_step(zeCommandListAppendMemoryCopy(list_, steps.data(), steps_, tables * sizeof(int32_t), nullptr,
                                              0, nullptr),
                "step count readback");
    for (uint32_t index = 0; index < tables; ++index) {
      DecodedTable& out = batch[index]->result;
      out.steps = steps[index];
      if (out.steps < 1 || out.steps > kMaxSteps) {
        throw InferenceDeviceError(std::format("table structure decoder: {} returned {} steps",
                                               description_, out.steps));
      }
      out.logits.resize(size_t(out.steps) * kVocab);
      out.location.resize(size_t(out.steps) * 8);
      const size_t base = size_t(index) * kMaxSteps;
      device_step(zeCommandListAppendMemoryCopy(list_, out.logits.data(),
                                                static_cast<float*>(logits_) + base * kVocab,
                                                out.logits.size() * sizeof(float), nullptr, 0, nullptr),
                  "logit readback");
      device_step(zeCommandListAppendMemoryCopy(list_, out.location.data(),
                                                static_cast<float*>(location_) + base * 8,
                                                out.location.size() * sizeof(float), nullptr, 0, nullptr),
                  "box readback");
    }
  }

  ze_driver_handle_t driver_ = nullptr;
  ze_device_handle_t device_ = nullptr;
  ze_context_handle_t context_ = nullptr;
  ze_module_handle_t module_ = nullptr;
  ze_kernel_handle_t kernel_ = nullptr;
  ze_command_list_handle_t list_ = nullptr;
  uint32_t max_groups_ = 0;
  uint32_t max_batch_ = 1;
  std::string description_;
  std::vector<void*> allocations_;
  void* weights_ = nullptr;
  void* features_ = nullptr;
  void* scratch_ = nullptr;
  void* exchange_ = nullptr;
  void* counters_ = nullptr;
  void* logits_ = nullptr;
  void* location_ = nullptr;
  void* steps_ = nullptr;

  std::mutex mutex_;
  std::condition_variable idle_;
  std::deque<Pending*> queue_;
  bool launching_ = false;
};

TableDecoder::TableDecoder(const std::filesystem::path& step_model, int gpu_index)
    : impl_(std::make_unique<Impl>(step_model, gpu_index)) {}

TableDecoder::~TableDecoder() = default;

DecodedTable TableDecoder::decode(std::span<const float> features) { return impl_->decode(features); }

const std::string& TableDecoder::description() const { return impl_->description(); }

TableDecoder& TableDecoder::shared(const std::filesystem::path& step_model, int gpu_index) {
  static std::mutex mutex;
  static std::map<std::pair<std::string, int>, std::unique_ptr<TableDecoder>> decoders;
  std::lock_guard lock(mutex);
  auto& slot = decoders[{step_model.string(), gpu_index}];
  if (slot == nullptr) slot = std::make_unique<TableDecoder>(step_model, gpu_index);
  return *slot;
}

}  // namespace grparse::levelzero
