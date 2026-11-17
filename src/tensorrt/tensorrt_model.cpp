#include "tensorrt_model.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <print>
#include <span>
#include <stdexcept>
#include <utility>

#include <NvInfer.h>

#include "../targets/sha256.h"
#include "engine_builder.h"
#include "engine_cache.h"
#include "shape_profiles.h"
#include "trt_common.h"

namespace grparse::trt {
namespace {

std::atomic<uint64_t> engines_built{0};
std::atomic<uint64_t> engines_loaded{0};

struct IoTensor {
  std::string name;
  TensorElement element = TensorElement::kFloat32;
};

TensorElement element_of(nvinfer1::DataType type, std::string_view what, const char* tensor) {
  switch (type) {
    case nvinfer1::DataType::kFLOAT: return TensorElement::kFloat32;
    case nvinfer1::DataType::kINT64: return TensorElement::kInt64;
    case nvinfer1::DataType::kUINT8: return TensorElement::kUInt8;
    default: break;
  }
  throw std::runtime_error(std::format(
      "gRParse {}: tensor {} has an element type gRParse does not read", what, tensor));
}

size_t element_count(std::span<const int64_t> shape) {
  size_t count = 1;
  for (const int64_t dim : shape) count *= static_cast<size_t>(dim);
  return count;
}

nvinfer1::Dims to_dims(const std::vector<int64_t>& shape) {
  nvinfer1::Dims dims{};
  dims.nbDims = static_cast<int32_t>(shape.size());
  for (size_t index = 0; index < shape.size(); ++index) dims.d[index] = shape[index];
  return dims;
}

std::string read_model_file(const std::filesystem::path& path, std::string_view what) {
  std::ifstream in(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!in.good() && !in.eof()) bytes.clear();
  if (bytes.empty()) {
    throw std::runtime_error(std::format("gRParse {}: cannot read model {}", what, path.string()));
  }
  return bytes;
}

std::string tensorrt_version() {
  return std::format("{}.{}.{}.{}", getInferLibMajorVersion(), getInferLibMinorVersion(),
                     getInferLibPatchVersion(), getInferLibBuildVersion());
}

// What one run call works with: a stream, the activation memory its
// contexts share (only one of them runs at a time), an execution context
// per optimization profile, and device buffers for the inputs and outputs.
// Members are destroyed in reverse order, so the contexts go first.
struct RunSlot {
  Stream stream;
  DeviceBuffer scratch;
  std::vector<DeviceBuffer> inputs;
  std::vector<DeviceBuffer> outputs;
  std::vector<std::unique_ptr<nvinfer1::IExecutionContext>> contexts;
};

// One engine and everything that runs on it.
class SharedEngine {
 public:
  explicit SharedEngine(const TensorRtModelSpec& spec);
  ~SharedEngine();
  SharedEngine(const SharedEngine&) = delete;
  SharedEngine& operator=(const SharedEngine&) = delete;

  std::vector<Tensor> run(std::span<const TensorView> inputs, size_t profile);

  const std::string& what() const { return what_; }
  const std::vector<ShapeProfile>& profiles() const { return profiles_; }
  const std::vector<IoTensor>& inputs() const { return inputs_; }
  const std::vector<std::string>& input_names() const { return input_names_; }
  const std::vector<std::string>& output_names() const { return output_names_; }

 private:
  void load(const TensorRtModelSpec& spec);
  void collect_io();
  std::unique_ptr<RunSlot> acquire();
  void release(std::unique_ptr<RunSlot> slot);
  nvinfer1::IExecutionContext& context_for(RunSlot& slot, size_t profile);
  std::vector<Tensor> infer(RunSlot& slot, std::span<const TensorView> inputs, size_t profile);

  std::string what_;
  int device_ = 0;
  // Read before anything selects the device, so a device index that does
  // not exist is reported as that and not as a failed selection.
  GpuIdentity gpu_;
  std::vector<ShapeProfile> profiles_;
  std::unique_ptr<nvinfer1::IRuntime> runtime_;
  std::unique_ptr<nvinfer1::ICudaEngine> engine_;
  std::vector<IoTensor> inputs_;
  std::vector<IoTensor> outputs_;
  std::vector<std::string> input_names_;
  std::vector<std::string> output_names_;
  std::mutex mutex_;  // the idle slots, and context creation on the engine
  std::vector<std::unique_ptr<RunSlot>> idle_;
};

SharedEngine::SharedEngine(const TensorRtModelSpec& spec)
    : what_(spec.what),
      device_(spec.cuda_device),
      gpu_(gpu_identity(spec.cuda_device)),
      profiles_(spec.profiles) {
  const DeviceGuard device(device_);
  load(spec);
  collect_io();
  // A model with only static inputs declares no profile; give the run path
  // the one its shapes imply so every model is chosen the same way.
  if (profiles_.empty()) {
    std::vector<std::vector<int64_t>> shapes;
    for (const IoTensor& input : inputs_) {
      const nvinfer1::Dims dims = engine_->getTensorShape(input.name.c_str());
      shapes.emplace_back(dims.d, dims.d + dims.nbDims);
    }
    profiles_.push_back(static_profile(std::move(shapes)));
  }
}

SharedEngine::~SharedEngine() {
  // Contexts and engine are freed with their device current.  A process
  // that lost its GPU skips the frees instead of terminating in them.
  try {
    const DeviceGuard device(device_);
    idle_.clear();
    engine_.reset();
    runtime_.reset();
  } catch (const std::exception&) {
    for (auto& slot : idle_) (void)slot.release();
    (void)engine_.release();
    (void)runtime_.release();
  }
}

void SharedEngine::load(const TensorRtModelSpec& spec) {
  std::string model = read_model_file(spec.model, what_);
  const EngineCacheKey key{
      .model_sha256 = targets::sha256_hex(model),
      .tensorrt_version = tensorrt_version(),
      .gpu_name = gpu_.name,
      .compute_capability = gpu_.compute_capability,
      .tf32 = spec.tf32,
      .profiles = spec.profiles,
  };
  const std::filesystem::path cache_dir = configured_engine_cache_dir();
  const std::string cache_name = engine_cache_file_name(spec.model.stem().string(), key);
  runtime_.reset(nvinfer1::createInferRuntime(logger()));
  if (runtime_ == nullptr) throw std::runtime_error("gRParse " + what_ + ": no TensorRT runtime");
  if (!cache_dir.empty()) {
    require_writable_cache_dir(cache_dir);
    if (const auto cached = read_cache_file(cache_dir, cache_name)) {
      engine_.reset(runtime_->deserializeCudaEngine(cached->data(), cached->size()));
      if (engine_ != nullptr) {
        engines_loaded.fetch_add(1);
        std::println("gRParse TensorRT: {} engine loaded from {}", what_,
                     (cache_dir / cache_name).string());
        return;
      }
      std::println(stderr, "gRParse TensorRT: {} is not a usable engine ({}); building it again",
                   (cache_dir / cache_name).string(), logger().take_errors());
    }
  }
  const auto started = std::chrono::steady_clock::now();
  const std::unique_ptr<nvinfer1::IHostMemory> plan = build_engine_plan(
      std::move(model), EngineRecipe{what_, spec.profiles, spec.tf32}, cache_dir);
  engine_.reset(runtime_->deserializeCudaEngine(plan->data(), plan->size()));
  if (engine_ == nullptr) {
    throw std::runtime_error(std::format("gRParse {}: TensorRT could not load the engine it built: {}",
                                         what_, logger().take_errors()));
  }
  engines_built.fetch_add(1);
  const auto seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::println("gRParse TensorRT: {} engine built in {:.1f} s ({} profile{}, {} on {})", what_,
               seconds, spec.profiles.size(), spec.profiles.size() == 1 ? "" : "s",
               key.tensorrt_version, gpu_.name);
  if (cache_dir.empty()) return;
  try {
    write_cache_file(cache_dir, cache_name, plan->data(), plan->size());
  } catch (const std::exception& error) {
    // The engine is built and loaded; a full volume costs the next start a
    // build, not this one its models.
    std::println(stderr, "gRParse TensorRT: {} engine not cached ({})", what_, error.what());
  }
}

void SharedEngine::collect_io() {
  for (int32_t index = 0; index < engine_->getNbIOTensors(); ++index) {
    const char* name = engine_->getIOTensorName(index);
    const IoTensor tensor{name, element_of(engine_->getTensorDataType(name), what_, name)};
    if (engine_->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT) {
      inputs_.push_back(tensor);
      input_names_.push_back(tensor.name);
    } else {
      outputs_.push_back(tensor);
      output_names_.push_back(tensor.name);
    }
  }
}

std::unique_ptr<RunSlot> SharedEngine::acquire() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!idle_.empty()) {
      std::unique_ptr<RunSlot> slot = std::move(idle_.back());
      idle_.pop_back();
      return slot;
    }
  }
  auto slot = std::make_unique<RunSlot>();
  slot->inputs = std::vector<DeviceBuffer>(inputs_.size());
  slot->outputs = std::vector<DeviceBuffer>(outputs_.size());
  slot->contexts.resize(profiles_.size());
  return slot;
}

void SharedEngine::release(std::unique_ptr<RunSlot> slot) {
  std::lock_guard<std::mutex> lock(mutex_);
  idle_.push_back(std::move(slot));
}

nvinfer1::IExecutionContext& SharedEngine::context_for(RunSlot& slot, size_t profile) {
  if (slot.contexts[profile] != nullptr) return *slot.contexts[profile];
  std::lock_guard<std::mutex> lock(mutex_);
  // The slot lends its contexts their activation memory, so each context
  // costs only its own state and the slot's memory is the largest profile
  // it has run, not the sum of them.
  std::unique_ptr<nvinfer1::IExecutionContext> context(engine_->createExecutionContext(
      nvinfer1::ExecutionContextAllocationStrategy::kUSER_MANAGED));
  if (context == nullptr ||
      !context->setOptimizationProfileAsync(static_cast<int32_t>(profile), slot.stream.get())) {
    throw InferenceDeviceError(std::format("TensorRT device failed {}: no execution context ({})",
                                           what_, logger().take_errors()));
  }
  slot.contexts[profile] = std::move(context);
  return *slot.contexts[profile];
}

std::vector<Tensor> SharedEngine::run(std::span<const TensorView> inputs, size_t profile) {
  const DeviceGuard device(device_);
  std::unique_ptr<RunSlot> slot = acquire();
  try {
    std::vector<Tensor> outputs = infer(*slot, inputs, profile);
    release(std::move(slot));
    return outputs;
  } catch (...) {
    // A slot that failed mid-run is not reused: its stream may still hold
    // work against buffers the next run would overwrite.
    cudaStreamSynchronize(slot->stream.get());
    throw;
  }
}

std::vector<Tensor> SharedEngine::infer(RunSlot& slot, std::span<const TensorView> inputs,
                                        size_t profile) {
  nvinfer1::IExecutionContext& context = context_for(slot, profile);
  const cudaStream_t stream = slot.stream.get();
  for (size_t index = 0; index < inputs.size(); ++index) {
    const char* name = inputs_[index].name.c_str();
    const size_t bytes =
        element_count(inputs[index].shape) * tensor_element_size(inputs[index].element);
    void* device = slot.inputs[index].reserve(bytes);
    if (!context.setInputShape(name, to_dims(inputs[index].shape)) ||
        !context.setTensorAddress(name, device)) {
      throw std::invalid_argument(std::format("gRParse {}: TensorRT refused input {} of shape {} ({})",
                                              what_, name, shape_text(inputs[index].shape),
                                              logger().take_errors()));
    }
    check_cuda(cudaMemcpyAsync(device, inputs[index].data, bytes, cudaMemcpyHostToDevice, stream),
               "input upload");
  }
  const int64_t scratch = engine_->getDeviceMemorySizeForProfileV2(static_cast<int32_t>(profile));
  context.setDeviceMemoryV2(slot.scratch.reserve(static_cast<size_t>(scratch)), scratch);

  std::vector<Tensor> results(outputs_.size());
  for (size_t index = 0; index < outputs_.size(); ++index) {
    const char* name = outputs_[index].name.c_str();
    const nvinfer1::Dims dims = context.getTensorShape(name);
    Tensor& result = results[index];
    result.element = outputs_[index].element;
    result.shape.assign(dims.d, dims.d + (dims.nbDims < 0 ? 0 : dims.nbDims));
    for (const int64_t dim : result.shape) {
      if (dim >= 0) continue;
      throw std::runtime_error(std::format(
          "gRParse {}: output {} has a shape TensorRT only knows after the run", what_, name));
    }
    result.bytes.resize(element_count(result.shape) * tensor_element_size(result.element));
    if (!context.setTensorAddress(name, slot.outputs[index].reserve(result.bytes.size()))) {
      throw InferenceDeviceError(std::format("TensorRT device failed {}: cannot bind output {}",
                                             what_, name));
    }
  }
  if (!context.enqueueV3(stream)) {
    throw InferenceDeviceError(std::format("TensorRT device failed {}: {}", what_,
                                           logger().take_errors()));
  }
  try {
    for (size_t index = 0; index < results.size(); ++index) {
      check_cuda(cudaMemcpyAsync(results[index].bytes.data(), slot.outputs[index].data(),
                                 results[index].bytes.size(), cudaMemcpyDeviceToHost, stream),
                 "output download");
    }
    check_cuda(cudaStreamSynchronize(stream), "synchronize");
  } catch (const std::runtime_error& error) {
    throw InferenceDeviceError(std::format("TensorRT device failed {}: {}", what_, error.what()));
  }
  return results;
}

// One handle on a shared engine: picks the profile, splits rows past the
// profile's largest batch, and joins the pieces.
class TensorRtModel final : public InferenceModel {
 public:
  explicit TensorRtModel(std::shared_ptr<SharedEngine> engine) : engine_(std::move(engine)) {}

  std::vector<Tensor> run(std::span<const TensorView> inputs) override;
  void prewarm(std::span<const std::vector<TensorSpec>> input_sets) override;
  const std::vector<std::string>& input_names() const override { return engine_->input_names(); }
  const std::vector<std::string>& output_names() const override { return engine_->output_names(); }
  std::string_view backend_name() const override { return "tensorrt"; }

 private:
  std::vector<Tensor> run_split(std::span<const TensorView> inputs, const ProfileChoice& choice,
                                int64_t rows);

  std::shared_ptr<SharedEngine> engine_;
};

std::vector<Tensor> TensorRtModel::run(std::span<const TensorView> inputs) {
  const std::vector<IoTensor>& expected = engine_->inputs();
  if (inputs.size() != expected.size()) {
    throw std::invalid_argument("gRParse " + engine_->what() + ": model input count mismatch");
  }
  std::vector<std::vector<int64_t>> shapes;
  shapes.reserve(inputs.size());
  for (size_t index = 0; index < inputs.size(); ++index) {
    if (inputs[index].element != expected[index].element) {
      throw std::invalid_argument(std::format("gRParse {}: input {} has the wrong element type",
                                              engine_->what(), expected[index].name));
    }
    shapes.push_back(inputs[index].shape);
  }
  const ProfileChoice choice = choose_profile(engine_->profiles(), shapes, engine_->what());
  const int64_t rows = shapes.front().empty() ? 1 : shapes.front().front();
  if (rows <= choice.max_rows) return engine_->run(inputs, choice.profile);
  return run_split(inputs, choice, rows);
}

std::vector<Tensor> TensorRtModel::run_split(std::span<const TensorView> inputs,
                                             const ProfileChoice& choice, int64_t rows) {
  for (const TensorView& input : inputs) {
    if (input.shape.empty() || input.shape.front() != rows) {
      throw std::invalid_argument("gRParse " + engine_->what() +
                                  ": inputs of a split run must share their row count");
    }
  }
  std::vector<Tensor> joined;
  for (int64_t offset = 0; offset < rows; offset += choice.max_rows) {
    const int64_t count = std::min(choice.max_rows, rows - offset);
    std::vector<TensorView> piece(inputs.begin(), inputs.end());
    for (TensorView& view : piece) {
      const size_t row_bytes = element_count(view.shape) / static_cast<size_t>(rows) *
                               tensor_element_size(view.element);
      view.data = static_cast<const std::byte*>(view.data) + row_bytes * static_cast<size_t>(offset);
      view.shape.front() = count;
    }
    std::vector<Tensor> part = engine_->run(piece, choice.profile);
    if (joined.empty()) {
      joined = std::move(part);
      continue;
    }
    for (size_t index = 0; index < joined.size(); ++index) {
      if (part[index].shape.empty() || part[index].shape.front() != count) {
        throw std::runtime_error("gRParse " + engine_->what() +
                                 ": an output of a split run does not carry rows first");
      }
      joined[index].shape.front() += count;
      joined[index].bytes.insert(joined[index].bytes.end(), part[index].bytes.begin(),
                                 part[index].bytes.end());
    }
  }
  return joined;
}

void TensorRtModel::prewarm(std::span<const std::vector<TensorSpec>> input_sets) {
  for (const std::vector<TensorSpec>& specs : input_sets) {
    std::vector<std::vector<std::byte>> zeros(specs.size());
    std::vector<TensorView> views;
    views.reserve(specs.size());
    for (size_t index = 0; index < specs.size(); ++index) {
      zeros[index].assign(element_count(specs[index].shape) * tensor_element_size(specs[index].element),
                          std::byte{0});
      views.push_back({specs[index].element, zeros[index].data(), specs[index].shape});
    }
    (void)run(views);
  }
}

std::string registry_key(const TensorRtModelSpec& spec) {
  EngineCacheKey shapes;
  shapes.profiles = spec.profiles;
  shapes.tf32 = spec.tf32;
  return std::format("{}|{}|{}", spec.model.string(), spec.cuda_device, engine_cache_key_text(shapes));
}

}  // namespace

std::unique_ptr<InferenceModel> make_tensorrt_model(const TensorRtModelSpec& spec) {
  // Engines build one at a time: the builder times kernels on the GPU, and
  // two builds at once would time each other.
  // The registry is never destroyed: static destructors run after CUDA's own
  // exit handlers, and freeing an engine then is undefined.  The process
  // exit reclaims the GPU.
  static std::mutex mutex;
  static auto* const engines = new std::map<std::string, std::shared_ptr<SharedEngine>>();
  std::lock_guard<std::mutex> lock(mutex);
  std::shared_ptr<SharedEngine>& engine = (*engines)[registry_key(spec)];
  if (engine == nullptr) engine = std::make_shared<SharedEngine>(spec);
  return std::make_unique<TensorRtModel>(engine);
}

bool device_present(int cuda_device, std::string* detail) {
  try {
    (void)gpu_identity(cuda_device);
    return true;
  } catch (const std::exception& error) {
    if (detail != nullptr) *detail = error.what();
    return false;
  }
}

EngineCounts engine_counts() { return EngineCounts{engines_built.load(), engines_loaded.load()}; }

}  // namespace grparse::trt
