#include "grparse_inference_model.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <numeric>
#include <print>
#include <utility>

#ifdef GRPARSE_NATIVE_OPENVINO
#include <openvino/c/openvino.h>
#endif

namespace grparse {
namespace {

std::atomic<uint64_t> device_errors{0};
std::atomic<uint64_t> runtime_compiles{0};

size_t element_count_of(const std::vector<int64_t>& shape) {
  return std::accumulate(shape.begin(), shape.end(), size_t{1},
                         [](size_t total, int64_t dim) { return total * static_cast<size_t>(dim); });
}

ONNXTensorElementDataType ort_element(TensorElement element) {
  switch (element) {
    case TensorElement::kFloat32: return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    case TensorElement::kInt64: return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
    case TensorElement::kUInt8: return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
  }
  throw std::invalid_argument("unknown tensor element");
}

TensorElement from_ort_element(ONNXTensorElementDataType type) {
  switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: return TensorElement::kFloat32;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: return TensorElement::kInt64;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8: return TensorElement::kUInt8;
    default: throw std::runtime_error("model output has an element type gRParse does not read");
  }
}

// ONNX Runtime: the session make_session / make_rapidocr_session build, run
// exactly as the engines ran it before the interface existed.
class OrtModel final : public InferenceModel {
 public:
  explicit OrtModel(Ort::Session session) : owned_(std::move(session)), session_(&owned_) {
    collect_names();
  }
  // RapidOCR nets keep their own Ort::Env; the session pointer they hand
  // over is owned here from now on.
  explicit OrtModel(Ort::Session* session) : heap_(session), session_(session) {
    collect_names();
  }

  std::vector<Tensor> run(std::span<const TensorView> inputs) override {
    if (inputs.size() != input_names_.size()) {
      throw std::invalid_argument("model input count mismatch");
    }
    const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<Ort::Value> values;
    values.reserve(inputs.size());
    for (const TensorView& input : inputs) {
      const size_t bytes = element_count_of(input.shape) * tensor_element_size(input.element);
      values.push_back(Ort::Value::CreateTensor(memory, const_cast<void*>(input.data), bytes,
                                                input.shape.data(), input.shape.size(),
                                                ort_element(input.element)));
    }
    auto outputs = session_->Run(Ort::RunOptions{nullptr}, input_pointers_.data(), values.data(),
                                 values.size(), output_pointers_.data(), output_pointers_.size());
    std::vector<Tensor> result;
    result.reserve(outputs.size());
    for (Ort::Value& output : outputs) {
      const auto info = output.GetTensorTypeAndShapeInfo();
      Tensor tensor;
      tensor.element = from_ort_element(info.GetElementType());
      tensor.shape = info.GetShape();
      const size_t bytes = element_count_of(tensor.shape) * tensor_element_size(tensor.element);
      const auto* data = static_cast<const std::byte*>(output.GetTensorRawData());
      tensor.bytes.assign(data, data + bytes);
      result.push_back(std::move(tensor));
    }
    return result;
  }

  void prewarm(std::span<const std::vector<TensorSpec>>) override {}
  const std::vector<std::string>& input_names() const override { return input_names_; }
  const std::vector<std::string>& output_names() const override { return output_names_; }
  std::string_view backend_name() const override { return "onnxruntime"; }

 private:
  void collect_names() {
    Ort::AllocatorWithDefaultOptions allocator;
    for (size_t i = 0; i < session_->GetInputCount(); ++i) {
      input_names_.emplace_back(session_->GetInputNameAllocated(i, allocator).get());
    }
    for (size_t i = 0; i < session_->GetOutputCount(); ++i) {
      output_names_.emplace_back(session_->GetOutputNameAllocated(i, allocator).get());
    }
    for (const auto& name : input_names_) input_pointers_.push_back(name.c_str());
    for (const auto& name : output_names_) output_pointers_.push_back(name.c_str());
  }

  Ort::Session owned_{nullptr};
  std::unique_ptr<Ort::Session> heap_;
  Ort::Session* session_ = nullptr;
  std::vector<std::string> input_names_;
  std::vector<std::string> output_names_;
  std::vector<const char*> input_pointers_;
  std::vector<const char*> output_pointers_;
};

#ifdef GRPARSE_NATIVE_OPENVINO

std::string last_error(ov_status_e status) {
  const char* detail = ov_get_last_err_msg();
  std::string message = ov_get_error_info(status);
  if (detail != nullptr && *detail != '\0') message += std::string(": ") + detail;
  return message;
}

template <typename T, void (*Free)(T*)>
using Handle = std::unique_ptr<T, decltype(Free)>;

ov_element_type_e ov_element(TensorElement element) {
  switch (element) {
    case TensorElement::kFloat32: return F32;
    case TensorElement::kInt64: return I64;
    case TensorElement::kUInt8: return U8;
  }
  throw std::invalid_argument("unknown tensor element");
}

TensorElement from_ov_element(ov_element_type_e type) {
  switch (type) {
    case F32: return TensorElement::kFloat32;
    case I64: return TensorElement::kInt64;
    case U8: return TensorElement::kUInt8;
    default: throw std::runtime_error("model output has an element type gRParse does not read");
  }
}

// One OpenVINO core per process: it owns the GPU context, the plugin and the
// compiled-kernel cache.
class NativeCore {
 public:
  // The live core.  After a device loss the core is replaced (the old one
  // is never freed: see lose()).
  static NativeCore& instance() {
    std::lock_guard<std::mutex> lock(mutex());
    if (current() == nullptr) current() = new NativeCore();
    return *current();
  }

  // A device loss poisons the core's GPU context: OpenVINO objects that
  // belong to it throw from their destructors (ov::Exception through the C
  // API's free functions, which std::terminate()s the process - the 2026-11-16
  // B70 crash).  So the core and everything compiled on it are leaked on
  // purpose, and the next instance() builds a fresh core and context.
  static void lose(const NativeCore* lost) {
    std::lock_guard<std::mutex> lock(mutex());
    if (current() == lost) current() = nullptr;
  }
  ov_core_t* get() const { return core_.get(); }
  // The device's default remote context (USM host tensors), or null when the
  // device has none (the CPU device used for accuracy checks).
  ov_remote_context_t* context() const {
    std::call_once(context_once_, [this] {
      ov_remote_context_t* context = nullptr;
      if (ov_core_get_default_context(core_.get(), device_.c_str(), &context) == OK) context_ = context;
    });
    return context_;
  }
  const std::string& device() const { return device_; }
  const std::string& precision() const { return precision_; }

 private:
  NativeCore() {
    ov_core_t* core = nullptr;
    if (const ov_status_e status = ov_core_create(&core); status != OK) {
      throw std::runtime_error("OpenVINO core: " + last_error(status));
    }
    core_.reset(core);
    const OrtEpSelection selection = ort_ep_selection();
    device_ = selection.openvino_device;
    if (!selection.openvino_cache_dir.empty()) {
      const ov_status_e status = ov_core_set_property(core_.get(), device_.c_str(),
                                                      ov_property_key_cache_dir,
                                                      selection.openvino_cache_dir.c_str());
      if (status != OK) throw std::runtime_error("OpenVINO cache_dir: " + last_error(status));
    }
    // FP16 on the GPU: the XMX engines' native type, and on the Arc Pro B70
    // (xe, OpenVINO 2025.4) the FP32 path miscomputes RT-DETRv2 heron and
    // faults the device on the figure classifier (CAT error, engine reset).
    const char* configured = std::getenv("GRPARSE_OPENVINO_PRECISION");
    precision_ = configured == nullptr || *configured == '\0' ? "f16" : configured;
    if (precision_ != "f16" && precision_ != "f32") {
      throw std::invalid_argument("GRPARSE_OPENVINO_PRECISION must be f16 or f32");
    }
  }

  static std::mutex& mutex() {
    static std::mutex lock;
    return lock;
  }
  static NativeCore*& current() {
    static NativeCore* core = nullptr;
    return core;
  }

  // Never freed (see lose()): the process exit reclaims it.
  std::unique_ptr<ov_core_t, void (*)(ov_core_t*)> core_{nullptr, [](ov_core_t*) {}};
  std::string device_;
  std::string precision_;
  mutable std::once_flag context_once_;
  mutable ov_remote_context_t* context_ = nullptr;  // leaked with the core
};

// The compiled model for one set of static input shapes, with its pool of
// infer requests.  Shared so a request in flight keeps it alive while a
// device error drops it from the model's map.
// One infer request with its input tensors: USM host allocations from the
// device's own context, made once and bound once, which every run copies
// its inputs into.  Host-pointer tensors would hand OpenVINO the caller's
// heap memory to wrap (a userptr VM_BIND per buffer), the path
// intel/compute-runtime#1010 shows faulting the B70 under memory pressure.
struct PooledRequest {
  ov_infer_request_t* request = nullptr;
  std::vector<ov_tensor_t*> inputs;
};

// The compiled model for one set of static input shapes, with its pool of
// infer requests.  Shared so a request in flight keeps it alive while a
// device error drops it from the model's map.
struct CompiledShape {
  ov_compiled_model_t* compiled = nullptr;
  const NativeCore* core = nullptr;
  std::vector<TensorSpec> inputs;
  std::mutex pool_mutex;
  std::vector<PooledRequest> idle;
  // Set when the device was lost under this shape: its objects are leaked
  // instead of freed (see NativeCore::lose).
  std::atomic<bool> lost{false};

  ~CompiledShape() {
    if (lost.load()) return;
    for (PooledRequest& pooled : idle) {
      ov_infer_request_free(pooled.request);
      for (ov_tensor_t* tensor : pooled.inputs) ov_tensor_free(tensor);
    }
    if (compiled != nullptr) ov_compiled_model_free(compiled);
  }

  PooledRequest acquire() {
    {
      std::lock_guard<std::mutex> lock(pool_mutex);
      if (!idle.empty()) {
        PooledRequest pooled = idle.back();
        idle.pop_back();
        return pooled;
      }
    }
    PooledRequest pooled;
    if (const ov_status_e status = ov_compiled_model_create_infer_request(compiled, &pooled.request);
        status != OK) {
      throw InferenceDeviceError("OpenVINO infer request: " + last_error(status));
    }
    ov_remote_context_t* context = core->context();
    for (size_t i = 0; i < inputs.size(); ++i) {
      ov_shape_t dims{};
      ov_shape_create(static_cast<int64_t>(inputs[i].shape.size()), inputs[i].shape.data(), &dims);
      ov_tensor_t* tensor = nullptr;
      const ov_status_e status =
          context != nullptr
              ? ov_remote_context_create_host_tensor(context, ov_element(inputs[i].element), dims, &tensor)
              : ov_tensor_create(ov_element(inputs[i].element), dims, &tensor);
      ov_shape_free(&dims);
      if (status != OK) throw InferenceDeviceError("OpenVINO input tensor: " + last_error(status));
      pooled.inputs.push_back(tensor);
      if (const ov_status_e set = ov_infer_request_set_input_tensor_by_index(pooled.request, i, tensor);
          set != OK) {
        throw InferenceDeviceError("OpenVINO input binding: " + last_error(set));
      }
    }
    return pooled;
  }

  void release(const PooledRequest& pooled) {
    if (lost.load()) return;  // leaked with the dead context
    std::lock_guard<std::mutex> lock(pool_mutex);
    idle.push_back(pooled);
  }
};

class NativeOpenVinoModel final : public InferenceModel {
 public:
  NativeOpenVinoModel(std::filesystem::path ir, std::string what, ModelShapePolicy shapes)
      : ir_(std::move(ir)), what_(std::move(what)), shapes_(std::move(shapes)) {
    if (!std::filesystem::is_regular_file(ir_)) {
      throw std::runtime_error("gRParse " + what_ + ": OpenVINO IR " + ir_.string() +
                               " is missing (the openvino flavor loads IR, not ONNX)");
    }
    Handle<ov_model_t, ov_model_free> model = read_model();
    size_t inputs = 0;
    size_t outputs = 0;
    ov_model_inputs_size(model.get(), &inputs);
    ov_model_outputs_size(model.get(), &outputs);
    for (size_t i = 0; i < inputs; ++i) {
      ov_output_const_port_t* port = nullptr;
      if (ov_model_const_input_by_index(model.get(), i, &port) != OK) {
        throw std::runtime_error("gRParse " + what_ + ": unreadable IR input");
      }
      char* name = nullptr;
      ov_port_get_any_name(port, &name);
      input_names_.emplace_back(name == nullptr ? "" : name);
      if (name != nullptr) ov_free(name);
      ov_output_const_port_free(port);
    }
    for (size_t i = 0; i < outputs; ++i) {
      ov_output_const_port_t* port = nullptr;
      if (ov_model_const_output_by_index(model.get(), i, &port) != OK) {
        throw std::runtime_error("gRParse " + what_ + ": unreadable IR output");
      }
      char* name = nullptr;
      ov_port_get_any_name(port, &name);
      output_names_.emplace_back(name == nullptr ? "" : name);
      if (name != nullptr) ov_free(name);
      ov_output_const_port_free(port);
    }
  }

  std::vector<Tensor> run(std::span<const TensorView> inputs) override {
    if (inputs.size() != input_names_.size()) {
      throw std::invalid_argument("gRParse " + what_ + ": model input count mismatch");
    }
    // A fixed batch: pad the rows up to the ladder with zeros, read back only
    // the real ones.
    const int64_t rows = inputs.front().shape.empty() ? 1 : inputs.front().shape.front();
    const int64_t batch = batch_bucket(rows);
    std::vector<std::vector<std::byte>> padded(inputs.size());
    std::vector<TensorView> views(inputs.begin(), inputs.end());
    if (batch != rows) {
      for (size_t i = 0; i < views.size(); ++i) {
        const size_t row_bytes = element_count_of(views[i].shape) / static_cast<size_t>(rows) *
                                 tensor_element_size(views[i].element);
        // Extra rows repeat the last real row: a valid sample for every
        // input (heron's orig_target_sizes included), never read back.
        padded[i].resize(row_bytes * static_cast<size_t>(batch));
        const auto* source = static_cast<const std::byte*>(views[i].data);
        std::memcpy(padded[i].data(), source, row_bytes * static_cast<size_t>(rows));
        for (int64_t row = rows; row < batch; ++row) {
          std::memcpy(padded[i].data() + row_bytes * static_cast<size_t>(row),
                      source + row_bytes * static_cast<size_t>(rows - 1), row_bytes);
        }
        views[i].data = padded[i].data();
        views[i].shape.front() = batch;
      }
    }
    std::shared_ptr<CompiledShape> shape = compiled_for(views, /*warming=*/false);
    PooledRequest pooled = shape->acquire();
    ov_infer_request_t* request = pooled.request;
    struct Releaser {
      CompiledShape& shape;
      const PooledRequest& pooled;
      bool keep = true;
      // A request that failed on the device is leaked, never freed: its
      // destructor would throw from a dead context.
      ~Releaser() {
        if (keep) shape.release(pooled);
      }
    } releaser{*shape, pooled};
    for (size_t i = 0; i < views.size(); ++i) {
      void* destination = nullptr;
      if (const ov_status_e status = ov_tensor_data(pooled.inputs[i], &destination); status != OK) {
        throw std::runtime_error("gRParse " + what_ + ": " + last_error(status));
      }
      std::memcpy(destination, views[i].data,
                  element_count_of(views[i].shape) * tensor_element_size(views[i].element));
    }
    if (const ov_status_e status = ov_infer_request_infer(request); status != OK) {
      releaser.keep = false;
      fail_device(*shape, last_error(status));
    }
    std::vector<Tensor> result;
    result.reserve(output_names_.size());
    for (size_t i = 0; i < output_names_.size(); ++i) {
      ov_tensor_t* raw = nullptr;
      if (const ov_status_e status = ov_infer_request_get_output_tensor_by_index(request, i, &raw);
          status != OK) {
        releaser.keep = false;
        fail_device(*shape, last_error(status));
      }
      Handle<ov_tensor_t, ov_tensor_free> output(raw, ov_tensor_free);
      ov_element_type_e type = UNDEFINED;
      ov_tensor_get_element_type(output.get(), &type);
      ov_shape_t dims{};
      ov_tensor_get_shape(output.get(), &dims);
      Tensor tensor;
      tensor.element = from_ov_element(type);
      tensor.shape.assign(dims.dims, dims.dims + dims.rank);
      ov_shape_free(&dims);
      void* data = nullptr;
      ov_tensor_data(output.get(), &data);
      size_t bytes = element_count_of(tensor.shape) * tensor_element_size(tensor.element);
      if (batch != rows && !tensor.shape.empty() && tensor.shape.front() == batch) {
        bytes = bytes / static_cast<size_t>(batch) * static_cast<size_t>(rows);
        tensor.shape.front() = rows;
      }
      const auto* begin = static_cast<const std::byte*>(data);
      tensor.bytes.assign(begin, begin + bytes);
      result.push_back(std::move(tensor));
    }
    return result;
  }

  void prewarm(std::span<const std::vector<TensorSpec>> input_sets) override {
    for (const auto& specs : input_sets) {
      std::vector<std::vector<std::byte>> zeros(specs.size());
      std::vector<TensorView> views;
      for (size_t i = 0; i < specs.size(); ++i) {
        zeros[i].assign(element_count_of(specs[i].shape) * tensor_element_size(specs[i].element),
                        std::byte{0});
        views.push_back({specs[i].element, zeros[i].data(), specs[i].shape});
      }
      compiled_for(views, /*warming=*/true);
      run(views);
    }
    warmed_ = true;
  }

  const std::vector<std::string>& input_names() const override { return input_names_; }
  const std::vector<std::string>& output_names() const override { return output_names_; }
  std::string_view backend_name() const override { return "openvino"; }

 private:
  Handle<ov_model_t, ov_model_free> read_model() const {
    // One IR read at a time, process-wide: concurrent reads of the same
    // .xml/.bin (two workers compiling new shapes) intermittently came back
    // with "Empty weights data in bin file" on OpenVINO 2025.4.
    static std::mutex read_mutex;
    std::lock_guard<std::mutex> lock(read_mutex);
    ov_model_t* model = nullptr;
    if (const ov_status_e status =
            ov_core_read_model(NativeCore::instance().get(), ir_.c_str(), nullptr, &model);
        status != OK) {
      throw std::runtime_error("gRParse " + what_ + ": OpenVINO could not read " + ir_.string() +
                               ": " + last_error(status));
    }
    return {model, ov_model_free};
  }

  int64_t batch_bucket(int64_t rows) const {
    for (const int64_t entry : shapes_.batch_ladder) {
      if (entry >= rows) return entry;
    }
    return rows;
  }

  static std::string key_of(std::span<const TensorView> views) {
    std::string key;
    for (const TensorView& view : views) {
      for (const int64_t dim : view.shape) key += std::to_string(dim) + "x";
      key += ";";
    }
    return key;
  }

  std::shared_ptr<CompiledShape> compiled_for(std::span<const TensorView> views, bool warming) {
    const std::string key = key_of(views);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (auto found = compiled_.find(key); found != compiled_.end()) return found->second;
    }
    // Compile outside the map lock; two threads racing on one new shape both
    // compile and the second insert loses, which only costs a duplicate.
    Handle<ov_model_t, ov_model_free> model = read_model();
    std::vector<ov_partial_shape_t> partial(views.size());
    std::vector<const char*> names;
    for (size_t i = 0; i < views.size(); ++i) {
      ov_partial_shape_create_static(static_cast<int64_t>(views[i].shape.size()),
                                     views[i].shape.data(), &partial[i]);
      names.push_back(input_names_[i].c_str());
    }
    const ov_status_e reshaped = ov_model_reshape(model.get(), names.data(), partial.data(), partial.size());
    for (auto& shape : partial) ov_partial_shape_free(&shape);
    if (reshaped != OK) {
      throw std::runtime_error("gRParse " + what_ + ": OpenVINO reshape to " + key + ": " +
                               last_error(reshaped));
    }
    const NativeCore& core = NativeCore::instance();
    ov_compiled_model_t* compiled = nullptr;
    const ov_status_e status = ov_core_compile_model(
        core.get(), model.get(), core.device().c_str(), 4, &compiled,
        ov_property_key_hint_inference_precision, core.precision().c_str(),
        ov_property_key_hint_performance_mode, "LATENCY");
    if (status != OK) {
      // No CPU anywhere: a model the device will not compile fails the
      // caller (at boot, during prewarm), naming it.
      throw std::runtime_error("gRParse " + what_ + ": the OpenVINO " + core.device() +
                               " device would not compile " + ir_.filename().string() + " at " +
                               key + ": " + last_error(status));
    }
    if (!warming && warmed_) {
      runtime_compiles.fetch_add(1);
      std::println(stderr, "gRParse {}: compiled shape {} after startup (not in the prewarm set)",
                   what_, key);
    }
    auto entry = std::make_shared<CompiledShape>();
    entry->compiled = compiled;
    entry->core = &core;
    for (const TensorView& view : views) entry->inputs.push_back({view.element, view.shape});
    std::lock_guard<std::mutex> lock(mutex_);
    return compiled_.try_emplace(key, std::move(entry)).first->second;
  }

  [[noreturn]] void fail_device(CompiledShape& failed, const std::string& message) {
    device_errors.fetch_add(1);
    {
      // Every compiled shape goes: after a device reset their kernels and
      // buffers belong to a dead context, so they are leaked (lost), never
      // freed, and the core is replaced.  The next call recompiles on a
      // fresh core (from the kernel cache).
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& [key, shape] : compiled_) shape->lost.store(true);
      failed.lost.store(true);
      compiled_.clear();
    }
    NativeCore::lose(failed.core);
    std::println(stderr, "gRParse {}: OpenVINO {} inference failed ({}); compiled shapes dropped",
                 what_, NativeCore::instance().device(), message);
    throw InferenceDeviceError("OpenVINO " + NativeCore::instance().device() + " device failed " +
                               what_ + ": " + message);
  }

  std::filesystem::path ir_;
  std::string what_;
  ModelShapePolicy shapes_;
  std::vector<std::string> input_names_;
  std::vector<std::string> output_names_;
  bool warmed_ = false;
  std::mutex mutex_;
  std::map<std::string, std::shared_ptr<CompiledShape>> compiled_;
};

// The IR beside the ONNX file: <models>/openvino/<stem>.xml.
std::filesystem::path ir_path(const std::filesystem::path& onnx) {
  return onnx.parent_path() / "openvino" / (onnx.stem().string() + ".xml");
}

#endif

}  // namespace

size_t tensor_element_size(TensorElement element) {
  switch (element) {
    case TensorElement::kFloat32: return sizeof(float);
    case TensorElement::kInt64: return sizeof(int64_t);
    case TensorElement::kUInt8: return sizeof(uint8_t);
  }
  return 0;
}

size_t Tensor::element_count() const { return element_count_of(shape); }

const float* Tensor::floats() const {
  if (element != TensorElement::kFloat32) throw std::logic_error("tensor is not float32");
  return reinterpret_cast<const float*>(bytes.data());
}

const int64_t* Tensor::int64s() const {
  if (element != TensorElement::kInt64) throw std::logic_error("tensor is not int64");
  return reinterpret_cast<const int64_t*>(bytes.data());
}

#ifdef GRPARSE_NATIVE_OPENVINO
std::unique_ptr<InferenceModel> make_remote_openvino_model(const std::filesystem::path& ir,
                                                           std::string what,
                                                           ModelShapePolicy shapes);

std::unique_ptr<InferenceModel> make_native_openvino_model(const std::filesystem::path& ir,
                                                           std::string what,
                                                           ModelShapePolicy shapes) {
  return std::make_unique<NativeOpenVinoModel>(ir, std::move(what), std::move(shapes));
}

namespace {
// In the server: a proxy to the inference host (or the model itself when
// isolation is off).  In the host, make_native_openvino_model runs.
std::unique_ptr<InferenceModel> openvino_model(const std::filesystem::path& onnx, std::string what,
                                               ModelShapePolicy shapes) {
  if (openvino_process_isolation()) {
    return make_remote_openvino_model(ir_path(onnx), std::move(what), std::move(shapes));
  }
  return make_native_openvino_model(ir_path(onnx), std::move(what), std::move(shapes));
}
}  // namespace
#endif

std::unique_ptr<InferenceModel> make_inference_model(const ModelSpec& spec) {
#ifdef GRPARSE_NATIVE_OPENVINO
  if (ort_ep_selection().ep == OrtEp::kOpenVino) {
    return openvino_model(spec.path, std::string(spec.what), spec.shapes);
  }
#endif
  static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-models");
  return std::make_unique<OrtModel>(
      make_session(env, spec.path, spec.what, spec.precision, spec.intra_op_threads));
}

std::unique_ptr<InferenceModel> make_rapidocr_model(Ort::Env& env, const std::filesystem::path& path,
                                                    OcrNet net, int num_thread) {
  const std::string what(ocr_net_name(net));
#ifdef GRPARSE_NATIVE_OPENVINO
  if (ort_ep_selection().ep == OrtEp::kOpenVino) {
    return openvino_model(path, what, ocr_net_shapes(net));
  }
#endif
  return std::make_unique<OrtModel>(make_rapidocr_session(env, path, what, num_thread));
}

std::unique_ptr<InferenceModel> wrap_ort_session(Ort::Session session) {
  return std::make_unique<OrtModel>(std::move(session));
}

uint64_t openvino_device_error_count() { return device_errors.load(); }
uint64_t openvino_runtime_compile_count() { return runtime_compiles.load(); }

}  // namespace grparse
