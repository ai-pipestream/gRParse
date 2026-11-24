#include "grparse_inference_model.h"

#include <algorithm>
#include <numeric>
#include <utility>

#ifdef GRPARSE_NATIVE_TENSORRT
#include "tensorrt/tensorrt_model.h"
#endif

namespace grparse {
namespace {

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

#ifdef GRPARSE_NATIVE_TENSORRT
namespace {
// Native TensorRT under a CUDA selection.  The engine builds (or loads from
// the engine cache) inside this call; a model that does not build is a
// refusal like any other provider's, counted and thrown, and nothing is
// built in its place.
std::unique_ptr<InferenceModel> tensorrt_model(const std::filesystem::path& onnx, std::string what,
                                               std::vector<ShapeProfile> profiles) {
  const OrtEpSelection selection = ort_ep_selection();
  std::unique_ptr<InferenceModel> model;
  try {
    model = trt::make_tensorrt_model(trt::TensorRtModelSpec{
        onnx, what, std::move(profiles), selection.cuda_device, cuda_tf32_enabled()});
  } catch (const std::exception& error) {
    throw tensorrt_refusal(what, onnx, error.what());
  }
  record_native_placement(what);  // reached only when the engine exists
  return model;
}
}  // namespace
#endif

std::unique_ptr<InferenceModel> make_inference_model(const ModelSpec& spec) {
#ifdef GRPARSE_NATIVE_TENSORRT
  if (ort_ep_selection().ep == OrtEp::kCuda) {
    return tensorrt_model(spec.path, std::string(spec.what), spec.shapes.profiles);
  }
#endif
  static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-models");
  return std::make_unique<OrtModel>(
      make_session(env, spec.path, spec.what, spec.precision, spec.intra_op_threads));
}

std::unique_ptr<InferenceModel> make_rapidocr_model(Ort::Env& env, const std::filesystem::path& path,
                                                    OcrNet net, int num_thread) {
  const std::string what(ocr_net_name(net));
#ifdef GRPARSE_NATIVE_TENSORRT
  if (ort_ep_selection().ep == OrtEp::kCuda) {
    return tensorrt_model(path, what, ocr_net_shapes(net).profiles);
  }
#endif
  return std::make_unique<OrtModel>(make_rapidocr_session(env, path, what, num_thread));
}

#ifndef GRPARSE_NATIVE_OPENVINO
bool native_openvino_build() { return false; }

std::unique_ptr<InferenceModel> make_openvino_model(const ModelSpec& spec) {
  throw std::runtime_error(std::string(spec.what) + ": this build carries no native OpenVINO");
}
#endif

bool tensorrt_device_present(int cuda_device, std::string* detail) {
#ifdef GRPARSE_NATIVE_TENSORRT
  return trt::device_present(cuda_device, detail);
#else
  (void)cuda_device;
  if (detail != nullptr) *detail = "this build carries no TensorRT";
  return false;
#endif
}

std::unique_ptr<InferenceModel> wrap_ort_session(Ort::Session session) {
  return std::make_unique<OrtModel>(std::move(session));
}

}  // namespace grparse
