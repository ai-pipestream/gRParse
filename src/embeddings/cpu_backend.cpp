#include "tensor_backend.h"

#include <array>
#include <stdexcept>
#include <onnxruntime_cxx_api.h>

namespace grparse::embedding::detail {
namespace {
class CpuBackend final : public TensorBackend {
 public:
  explicit CpuBackend(std::string_view model)
      : env_(ORT_LOGGING_LEVEL_WARNING, "grparse-embedding"),
        session_(nullptr) {
    env_.DisableTelemetryEvents();
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(2);
    options.SetInterOpNumThreads(1);
    // Deliberately no execution provider appended: CPU is a reference
    // backend even when the enclosing parser uses a GPU for vision.
    // ONNX parsing completes here; the session owns the loaded model state.
    session_ = Ort::Session(env_, model.data(), model.size(), options);
    if (session_.GetInputCount() != 3 || session_.GetOutputCount() != 1)
      throw std::runtime_error("embedding model must have three token inputs and one hidden-state output");
  }

  std::vector<float> infer(const TokenBatch& tokens) override {
    const std::array<int64_t, 2> shape{static_cast<int64_t>(tokens.rows),
                                       static_cast<int64_t>(tokens.columns)};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::array<Ort::Value, 3> inputs{
        Ort::Value::CreateTensor<int64_t>(memory, const_cast<int64_t*>(tokens.ids.data()), tokens.ids.size(), shape.data(), 2),
        Ort::Value::CreateTensor<int64_t>(memory, const_cast<int64_t*>(tokens.mask.data()), tokens.mask.size(), shape.data(), 2),
        Ort::Value::CreateTensor<int64_t>(memory, const_cast<int64_t*>(tokens.types.data()), tokens.types.size(), shape.data(), 2)};
    constexpr std::array<const char*, 3> names{"input_ids", "attention_mask", "token_type_ids"};
    Ort::AllocatorWithDefaultOptions allocator;
    auto output_name = session_.GetOutputNameAllocated(0, allocator);
    const char* output_names[]{output_name.get()};
    auto outputs = session_.Run(Ort::RunOptions{nullptr}, names.data(), inputs.data(), 3, output_names, 1);
    const auto info = outputs.front().GetTensorTypeAndShapeInfo();
    const std::vector<int64_t> expected{shape[0], shape[1], 384};
    if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || info.GetShape() != expected)
      throw std::runtime_error("embedding model returned unexpected hidden-state shape or precision");
    const float* data = outputs.front().GetTensorData<float>();
    return {data, data + info.GetElementCount()};
  }

 private:
  Ort::Env env_;
  Ort::Session session_;
};
}  // namespace

std::unique_ptr<TensorBackend> make_cpu_backend(std::string_view model) {
  return std::make_unique<CpuBackend>(model);
}
}  // namespace grparse::embedding::detail
