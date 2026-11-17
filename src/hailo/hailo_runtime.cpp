#include "hailo_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <print>
#include <stdexcept>
#include <utility>

#include <hailo/hailort.hpp>

namespace grparse::hailo {
namespace {

// Long enough for the largest batch through the slowest network on a busy
// device; a call that takes longer is a device failure, not a slow page.
constexpr std::chrono::milliseconds kInferTimeout{60000};
// One call, and one more after the model is loaded again.
constexpr int kDeviceAttempts = 2;

std::string status_text(hailo_status status) {
  const char* message = hailo_get_status_message(status);
  return std::format("{} (HailoRT status {})", message == nullptr ? "unknown" : message,
                     static_cast<int>(status));
}

// What one model has done on the device since start.
struct ModelCounters {
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> reloads{0};
};

struct Runtime {
  std::mutex mutex;
  std::shared_ptr<hailort::VDevice> vdevice;
  // A second handle on the same chip for identify and statistics; the
  // virtual device owns inference.
  std::unique_ptr<hailort::Device> control;
  DeviceIdentity identity;
  std::vector<std::pair<std::string, std::shared_ptr<ModelCounters>>> counters;
  // Read once, when first asked (after the models are loaded): the query
  // makes the chip's runtime sample itself for a tenth of a second and log
  // every part of the answer its firmware cannot give, so it is not repeated
  // on every scrape.
  std::optional<DeviceStats> stats;
};

Runtime& runtime() {
  static Runtime instance;
  return instance;
}

size_t frame_elements(const hailo_3d_image_shape_t& shape) {
  return static_cast<size_t>(shape.height) * shape.width * shape.features;
}

std::vector<int64_t> frame_shape(const hailo_3d_image_shape_t& shape) {
  return {static_cast<int64_t>(shape.height), static_cast<int64_t>(shape.width),
          static_cast<int64_t>(shape.features)};
}

class Model final : public HailoModel {
 public:
  Model(const HailoModelSpec& spec, std::shared_ptr<hailort::VDevice> vdevice,
        std::shared_ptr<ModelCounters> counters)
      : spec_(spec), vdevice_(std::move(vdevice)), counters_(std::move(counters)) {
    if (!std::filesystem::is_regular_file(spec_.hef)) {
      throw std::runtime_error(std::format("gRParse {}: the HEF is missing: {}", spec_.what,
                                           spec_.hef.string()));
    }
    load();
    for (const auto& input : infer_model_->inputs()) {
      input_names_.push_back(input.name());
      shapes_.inputs.push_back({TensorElement::kUInt8, frame_shape(input.shape())});
      input_frame_bytes_.push_back(frame_elements(input.shape()));
    }
    for (const auto& output : infer_model_->outputs()) {
      output_names_.push_back(output.name());
      shapes_.outputs.push_back({TensorElement::kFloat32, frame_shape(output.shape())});
      output_frame_bytes_.push_back(frame_elements(output.shape()) * sizeof(float));
      const hailo_format_type_t type = output.format().type;
      const std::vector<hailo_quant_info_t> quant = output.get_quant_infos();
      if ((type != HAILO_FORMAT_TYPE_UINT8 && type != HAILO_FORMAT_TYPE_UINT16) ||
          (quant.size() != 1 && quant.size() != output.shape().features) ||
          output.get_frame_size() !=
              frame_elements(output.shape()) * (type == HAILO_FORMAT_TYPE_UINT8 ? 1 : 2)) {
        throw std::runtime_error(std::format(
            "gRParse {}: output {} of {} is not a plain 8 or 16 bit quantized tensor", spec_.what,
            output.name(), spec_.hef.string()));
      }
      raw_outputs_.push_back(RawOutput{.wide = type == HAILO_FORMAT_TYPE_UINT16,
                                       .frame_bytes = output.get_frame_size(),
                                       .features = output.shape().features,
                                       .quant = quant});
    }
  }

  ~Model() override { (void)configured_.shutdown(); }

  std::vector<Tensor> run(std::span<const TensorView> inputs) override {
    if (inputs.size() != input_names_.size()) {
      throw std::invalid_argument(std::format("gRParse {}: {} inputs given, the HEF takes {}",
                                              spec_.what, inputs.size(), input_names_.size()));
    }
    const int64_t frames = inputs.empty() || inputs[0].shape.empty() ? 0 : inputs[0].shape[0];
    for (size_t i = 0; i < inputs.size(); ++i) {
      const auto& want = shapes_.inputs[i].shape;
      const auto& got = inputs[i].shape;
      if (inputs[i].element != TensorElement::kUInt8 || got.size() != 4 || got[0] != frames ||
          got[1] != want[0] || got[2] != want[1] || got[3] != want[2]) {
        throw std::invalid_argument(std::format(
            "gRParse {}: input {} must be uint8 [frames, {}, {}, {}]", spec_.what, input_names_[i],
            want[0], want[1], want[2]));
      }
    }
    std::vector<Tensor> outputs(output_names_.size());
    for (size_t i = 0; i < outputs.size(); ++i) {
      outputs[i].element = TensorElement::kFloat32;
      outputs[i].shape = {frames};
      outputs[i].shape.insert(outputs[i].shape.end(), shapes_.outputs[i].shape.begin(),
                              shapes_.outputs[i].shape.end());
      outputs[i].bytes.resize(static_cast<size_t>(frames) * output_frame_bytes_[i]);
    }
    if (frames == 0) return outputs;

    // One caller at a time, one device batch in flight at a time: the
    // device runs a single batch at once whatever is queued.
    const std::lock_guard<std::mutex> lock(run_mutex_);
    const int64_t batch = std::max<int64_t>(1, spec_.batch_size);
    // The chip sometimes aborts a model in the middle of a batch
    // (HAILO_STREAM_ABORT, HailoRT 5.1.1) and every later call on it then
    // fails.  The model is loaded onto the device again and the call runs
    // once more, on the device; a second failure is the call's failure.
    for (int attempt = 1;; ++attempt) {
      try {
        if (broken_) {
          reload();
          broken_ = false;
        }
        for (int64_t first = 0; first < frames; first += batch) {
          run_batch(inputs, outputs, first, std::min(batch, frames - first));
        }
        break;
      } catch (const InferenceDeviceError& error) {
        broken_ = true;
        if (attempt >= kDeviceAttempts) throw;
        std::println(stderr, "gRParse {}: {}; loading the model onto the device again", spec_.what,
                     error.what());
      }
    }
    counters_->frames.fetch_add(static_cast<uint64_t>(frames));
    return outputs;
  }

  void prewarm(std::span<const std::vector<TensorSpec>>) override {
    // A HEF has one shape; one frame of zeros loads every context once.
    std::vector<std::vector<uint8_t>> zeros;
    std::vector<TensorView> views;
    for (size_t i = 0; i < input_names_.size(); ++i) {
      zeros.emplace_back(input_frame_bytes_[i], uint8_t{0});
      std::vector<int64_t> shape{1};
      shape.insert(shape.end(), shapes_.inputs[i].shape.begin(), shapes_.inputs[i].shape.end());
      views.push_back({TensorElement::kUInt8, zeros.back().data(), std::move(shape)});
    }
    (void)run(views);
    // The warm-up frame is not work done for a document.
    counters_->frames.fetch_sub(1);
  }

  const std::vector<std::string>& input_names() const override { return input_names_; }
  const std::vector<std::string>& output_names() const override { return output_names_; }
  std::string_view backend_name() const override { return "Hailo"; }
  const HailoModelShapes& shapes() const override { return shapes_; }

 private:
  // Puts the HEF on the device: uint8 in (the chip normalizes), quantized
  // out.  Throws std::runtime_error with HailoRT's status.
  void load() {
    (void)configured_.shutdown();
    auto infer_model = vdevice_->create_infer_model(spec_.hef.string());
    if (!infer_model) {
      throw std::runtime_error(std::format("gRParse {}: the Hailo device refused {}: {}",
                                           spec_.what, spec_.hef.string(),
                                           status_text(infer_model.status())));
    }
    infer_model_ = infer_model.release();
    infer_model_->set_batch_size(spec_.batch_size);
    for (const auto& input : infer_model_->inputs()) {
      infer_model_->input(input.name())->set_format_type(HAILO_FORMAT_TYPE_UINT8);
    }
    // Outputs keep the chip's own 8 or 16 bit numbers and are turned into
    // floats here, on the host: asking HailoRT for float32 makes the chip's
    // small CPU do that and sends four times the bytes across PCIe.
    auto configured = infer_model_->configure();
    if (!configured) {
      throw std::runtime_error(std::format("gRParse {}: the Hailo device could not configure {}: {}",
                                           spec_.what, spec_.hef.string(),
                                           status_text(configured.status())));
    }
    configured_ = configured.release();
  }

  // load() for a model the device aborted; a device that will not take it
  // back is a device failure of the call that needed it.
  void reload() {
    counters_->reloads.fetch_add(1);
    try {
      load();
    } catch (const std::runtime_error& error) {
      throw InferenceDeviceError(std::format("Hailo device failed {}: reload: {}", spec_.what,
                                             error.what()));
    }
  }

  // Frames [first, first + count) as one device batch: one job, one wait.
  void run_batch(std::span<const TensorView> inputs, std::vector<Tensor>& outputs, int64_t first,
                 int64_t count) {
    std::vector<std::vector<uint8_t>> raw(outputs.size());
    for (size_t i = 0; i < outputs.size(); ++i) {
      raw[i].resize(static_cast<size_t>(count) * raw_outputs_[i].frame_bytes);
    }
    std::vector<hailort::ConfiguredInferModel::Bindings> batch;
    batch.reserve(static_cast<size_t>(count));
    for (int64_t frame = first; frame < first + count; ++frame) {
      auto bindings = configured_.create_bindings();
      if (!bindings) fail(std::format("create bindings: {}", status_text(bindings.status())));
      for (size_t i = 0; i < inputs.size(); ++i) {
        auto* data = const_cast<uint8_t*>(static_cast<const uint8_t*>(inputs[i].data)) +
                     static_cast<size_t>(frame) * input_frame_bytes_[i];
        check(bindings->input(input_names_[i])->set_buffer(
                  hailort::MemoryView(data, input_frame_bytes_[i])),
              "bind input");
      }
      for (size_t i = 0; i < outputs.size(); ++i) {
        auto* data = raw[i].data() + static_cast<size_t>(frame - first) * raw_outputs_[i].frame_bytes;
        check(bindings->output(output_names_[i])->set_buffer(
                  hailort::MemoryView(data, raw_outputs_[i].frame_bytes)),
              "bind output");
      }
      batch.push_back(bindings.release());
    }
    check(configured_.wait_for_async_ready(kInferTimeout, static_cast<uint32_t>(count)),
          "wait for the device queue");
    std::atomic<int> completion{HAILO_SUCCESS};
    auto job = configured_.run_async(batch,
                                     [&completion](const hailort::AsyncInferCompletionInfo& info) {
                                       if (info.status != HAILO_SUCCESS) completion.store(info.status);
                                     });
    if (!job) fail(std::format("queue a batch: {}", status_text(job.status())));
    // The job writes into `raw` and reports into `completion`; both outlive
    // it only if this waits, whatever the wait returns.
    const hailo_status waited = job->wait(kInferTimeout);
    if (waited != HAILO_SUCCESS) fail(std::format("inference: {}", status_text(waited)));
    if (completion.load() != HAILO_SUCCESS) {
      fail(std::format("inference: {}",
                       status_text(static_cast<hailo_status>(completion.load()))));
    }
    for (size_t i = 0; i < outputs.size(); ++i) {
      auto* floats = reinterpret_cast<float*>(outputs[i].bytes.data()) +
                     static_cast<size_t>(first) * (output_frame_bytes_[i] / sizeof(float));
      dequantize(raw_outputs_[i], raw[i].data(), static_cast<size_t>(count), floats);
    }
  }

  // One output as the device sends it.
  struct RawOutput {
    bool wide = false;       // 16 bit values; 8 bit otherwise
    size_t frame_bytes = 0;
    size_t features = 0;     // size of the last dimension
    // One entry for the whole tensor, or one per feature.
    std::vector<hailo_quant_info_t> quant;
  };

  // value = scale * (quantized - zero point), HailoRT's own rule.
  static void dequantize(const RawOutput& output, const uint8_t* raw, size_t frames, float* floats) {
    const size_t values = frames * output.frame_bytes / (output.wide ? 2 : 1);
    const auto* wide = reinterpret_cast<const uint16_t*>(raw);
    if (output.quant.size() == 1) {
      const float scale = output.quant[0].qp_scale;
      const float zero = output.quant[0].qp_zp;
      for (size_t i = 0; i < values; ++i) {
        floats[i] = scale * (static_cast<float>(output.wide ? wide[i] : raw[i]) - zero);
      }
      return;
    }
    for (size_t i = 0; i < values; ++i) {
      const hailo_quant_info_t& quant = output.quant[i % output.features];
      floats[i] = quant.qp_scale * (static_cast<float>(output.wide ? wide[i] : raw[i]) - quant.qp_zp);
    }
  }

  [[noreturn]] void fail(const std::string& detail) const {
    throw InferenceDeviceError(std::format("Hailo device failed {}: {}", spec_.what, detail));
  }
  void check(hailo_status status, std::string_view step) const {
    if (status != HAILO_SUCCESS) fail(std::format("{}: {}", step, status_text(status)));
  }

  HailoModelSpec spec_;
  std::shared_ptr<hailort::VDevice> vdevice_;
  std::shared_ptr<ModelCounters> counters_;
  std::shared_ptr<hailort::InferModel> infer_model_;
  hailort::ConfiguredInferModel configured_;
  std::vector<std::string> input_names_;
  std::vector<std::string> output_names_;
  HailoModelShapes shapes_;
  std::vector<size_t> input_frame_bytes_;
  std::vector<size_t> output_frame_bytes_;
  std::vector<RawOutput> raw_outputs_;
  std::mutex run_mutex_;
  bool broken_ = false;
};

}  // namespace

bool runtime_built() { return true; }

DeviceIdentity open_device() {
  Runtime& state = runtime();
  const std::lock_guard<std::mutex> lock(state.mutex);
  if (state.vdevice) return state.identity;
  auto ids = hailort::Device::scan();
  if (!ids) {
    throw std::runtime_error("no Hailo device: the HailoRT scan failed: " +
                             status_text(ids.status()));
  }
  if (ids->empty()) {
    throw std::runtime_error(
        "no Hailo device: HailoRT found none (is /dev/hailo0 present on the host and passed "
        "to the container?)");
  }
  const std::string id = ids->front();
  auto control = hailort::Device::create(id);
  if (!control) {
    throw std::runtime_error(std::format("Hailo device {} did not open: {}", id,
                                         status_text(control.status())));
  }
  auto identity = control.value()->identify();
  if (!identity) {
    throw std::runtime_error(std::format("Hailo device {} did not identify itself: {}", id,
                                         status_text(identity.status())));
  }
  if (identity->device_architecture != HAILO_ARCH_HAILO10H) {
    throw std::runtime_error(std::format(
        "Hailo device {} is not a Hailo-10H (architecture {}); the HEFs are built for hailo10h",
        id, static_cast<int>(identity->device_architecture)));
  }
  hailo_vdevice_params_t params;
  if (const hailo_status status = hailo_init_vdevice_params(&params); status != HAILO_SUCCESS) {
    throw std::runtime_error("HailoRT virtual device parameters: " + status_text(status));
  }
  auto vdevice = hailort::VDevice::create_shared(params);
  if (!vdevice) {
    throw std::runtime_error(std::format("Hailo device {} did not open for inference: {}", id,
                                         status_text(vdevice.status())));
  }
  state.identity = DeviceIdentity{
      .id = id,
      .architecture = "HAILO10H",
      .firmware = std::format("{}.{}.{}", identity->fw_version.major, identity->fw_version.minor,
                              identity->fw_version.revision),
  };
  state.control = control.release();
  state.vdevice = vdevice.release();
  return state.identity;
}

std::optional<DeviceStats> device_stats() {
  Runtime& state = runtime();
  const std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.control) return std::nullopt;
  if (state.stats) return state.stats;
  auto performance = state.control->query_performance_stats();
  if (!performance) return std::nullopt;
  // hailort.h documents the two RAM figures as bytes; firmware 5.1.1 answers
  // in KiB (HailoRT reads them from the chip's own `free`, 6087196 for a
  // module with 8 GB).  A total under 64 MiB cannot be bytes on any Hailo-10,
  // so such an answer is read as KiB.
  const int64_t unit = performance->ram_size_total < (int64_t{64} << 20) ? 1024 : 1;
  DeviceStats stats{
      .ram_total_bytes = performance->ram_size_total * unit,
      .ram_used_bytes = performance->ram_size_used * unit,
  };
  if (auto health = state.control->query_health_stats()) {
    stats.temperature_celsius = health->on_die_temperature;
  }
  state.stats = stats;
  return state.stats;
}

std::unique_ptr<HailoModel> make_hailo_model(const HailoModelSpec& spec) {
  Runtime& state = runtime();
  std::shared_ptr<hailort::VDevice> vdevice;
  std::shared_ptr<ModelCounters> counters;
  {
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.vdevice) {
      throw std::logic_error(std::format("gRParse {}: the Hailo device is not open", spec.what));
    }
    vdevice = state.vdevice;
    for (auto& [name, existing] : state.counters) {
      if (name == spec.what) counters = existing;
    }
    if (!counters) {
      counters = std::make_shared<ModelCounters>();
      state.counters.emplace_back(spec.what, counters);
    }
  }
  return std::make_unique<Model>(spec, std::move(vdevice), std::move(counters));
}

std::vector<ModelFrames> model_frame_counts() {
  Runtime& state = runtime();
  const std::lock_guard<std::mutex> lock(state.mutex);
  std::vector<ModelFrames> counts;
  counts.reserve(state.counters.size());
  for (const auto& [name, counters] : state.counters) {
    counts.push_back({name, counters->frames.load(), counters->reloads.load()});
  }
  return counts;
}

}  // namespace grparse::hailo
