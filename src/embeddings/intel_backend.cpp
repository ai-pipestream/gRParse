#include "tensor_backend.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <openvino/c/openvino.h>

namespace grparse::embedding::detail {
namespace {
void check(ov_status_e status) {
  if (status != OK) throw std::runtime_error("native OpenVINO embedding operation failed");
}

template <typename T, void (*Free)(T*)>
using Handle = std::unique_ptr<T, decltype(Free)>;

class IntelBackend final : public TensorBackend {
 public:
  IntelBackend(std::string_view model, const std::string& device) {
    const bool indexed_gpu = device.starts_with("GPU.") && device.size() > 4 &&
        std::all_of(device.begin() + 4, device.end(),
                    [](unsigned char c) { return c >= '0' && c <= '9'; });
    if (device != "GPU" && !indexed_gpu)
      throw std::invalid_argument("Intel embedding device must select an Intel GPU");
    ov_core_t* core = nullptr;
    check(ov_core_create(&core));
    core_.reset(core);
    ov_model_t* network = nullptr;
    // The pinned ONNX contains its weights. Compile while the verified bytes
    // are alive; the compiled model retains its own native model state.
    check(ov_core_read_model_from_memory_buffer(core_.get(), model.data(), model.size(), nullptr, &network));
    Handle<ov_model_t, ov_model_free> model_handle(network, ov_model_free);
    ov_compiled_model_t* compiled = nullptr;
    check(ov_core_compile_model(core_.get(), network, device.c_str(), 4, &compiled,
                               "INFERENCE_PRECISION_HINT", "f32", "PERFORMANCE_HINT", "LATENCY"));
    compiled_.reset(compiled);
    ov_infer_request_t* request = nullptr;
    check(ov_compiled_model_create_infer_request(compiled_.get(), &request));
    request_.reset(request);
  }

  std::vector<float> infer(const TokenBatch& tokens) override {
    std::array<int64_t, 2> dims{static_cast<int64_t>(tokens.rows), static_cast<int64_t>(tokens.columns)};
    const ov_shape_t shape{2, dims.data()};
    constexpr std::array<const char*, 3> names{"input_ids", "attention_mask", "token_type_ids"};
    const std::array<const std::vector<int64_t>*, 3> values{&tokens.ids, &tokens.mask, &tokens.types};
    for (std::size_t i = 0; i < names.size(); ++i) {
      ov_tensor_t* tensor = nullptr;
      check(ov_tensor_create_from_host_ptr(I64, shape, const_cast<int64_t*>(values[i]->data()), &tensor));
      Handle<ov_tensor_t, ov_tensor_free> input(tensor, ov_tensor_free);
      check(ov_infer_request_set_tensor(request_.get(), names[i], input.get()));
    }
    check(ov_infer_request_infer(request_.get()));
    ov_tensor_t* tensor = nullptr;
    check(ov_infer_request_get_output_tensor_by_index(request_.get(), 0, &tensor));
    Handle<ov_tensor_t, ov_tensor_free> output(tensor, ov_tensor_free);
    ov_element_type_e type = UNDEFINED;
    check(ov_tensor_get_element_type(output.get(), &type));
    ov_shape_t actual{};
    check(ov_tensor_get_shape(output.get(), &actual));
    const bool shape_ok = actual.rank == 3 && actual.dims[0] == dims[0] &&
                          actual.dims[1] == dims[1] && actual.dims[2] == 384;
    ov_shape_free(&actual);
    if (type != F32 || !shape_ok)
      throw std::runtime_error("Intel embedding output shape or precision mismatch");
    void* raw = nullptr;
    check(ov_tensor_data(output.get(), &raw));
    const float* data = static_cast<const float*>(raw);
    return {data, data + tokens.rows * tokens.columns * 384};
  }

 private:
  // The C interface isolates the SDK wheel's old C++ library ABI from the
  // parser's ABI without routing inference through another runtime.
  Handle<ov_core_t, ov_core_free> core_{nullptr, ov_core_free};
  Handle<ov_compiled_model_t, ov_compiled_model_free> compiled_{nullptr, ov_compiled_model_free};
  Handle<ov_infer_request_t, ov_infer_request_free> request_{nullptr, ov_infer_request_free};
};
}  // namespace

std::unique_ptr<TensorBackend> make_intel_backend(std::string_view model, const std::string& device) {
  return std::make_unique<IntelBackend>(model, device);
}
}  // namespace grparse::embedding::detail
