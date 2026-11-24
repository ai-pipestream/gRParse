// Native OpenVINO behind InferenceModel: the model file read by OpenVINO's
// own frontend and compiled by its device plugin, with no ONNX Runtime
// session anywhere.  Built only into the Intel image (GRPARSE_NATIVE_OPENVINO,
// cmake/Embeddings.cmake), through the C API, which is ABI-neutral against
// the runtime the image ships.

#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openvino/c/openvino.h>

#include "grparse_inference_model.h"
#include "grparse_session_ep.h"

namespace grparse {
namespace {

template <typename T, void (*Free)(T*)>
using Handle = std::unique_ptr<T, decltype(Free)>;

// OpenVINO's own words for the last failure on this thread.
std::string ov_error(ov_status_e status) {
  const char* detail = ov_get_last_err_msg();
  return std::format("{} ({})", ov_get_error_info(status),
                     detail == nullptr || *detail == '\0' ? "no detail" : detail);
}

void check(ov_status_e status, std::string_view what, std::string_view step) {
  if (status != OK) {
    throw InferenceDeviceError(std::format("{}: OpenVINO {} failed: {}", what, step, ov_error(status)));
  }
}

ov_element_type_e ov_element(TensorElement element) {
  switch (element) {
    case TensorElement::kFloat32: return F32;
    case TensorElement::kInt64: return I64;
    case TensorElement::kUInt8: return U8;
  }
  return UNDEFINED;
}

TensorElement tensor_element(ov_element_type_e element, std::string_view what) {
  switch (element) {
    case F32: return TensorElement::kFloat32;
    case I64: return TensorElement::kInt64;
    case U8: return TensorElement::kUInt8;
    default:
      throw std::runtime_error(std::format("{}: OpenVINO output has an unsupported element type", what));
  }
}

// One core per process: it owns the loaded device plugins, which every
// compiled model shares.
ov_core_t* process_core() {
  static const Handle<ov_core_t, ov_core_free> core = [] {
    ov_core_t* created = nullptr;
    check(ov_core_create(&created), "OpenVINO", "core creation");
    return Handle<ov_core_t, ov_core_free>(created, ov_core_free);
  }();
  return core.get();
}

std::vector<std::string> port_names(const ov_compiled_model_t* compiled, bool inputs,
                                     std::string_view what) {
  size_t count = 0;
  check(inputs ? ov_compiled_model_inputs_size(compiled, &count)
               : ov_compiled_model_outputs_size(compiled, &count),
        what, "port count");
  std::vector<std::string> names;
  for (size_t index = 0; index < count; ++index) {
    ov_output_const_port_t* port = nullptr;
    check(inputs ? ov_compiled_model_input_by_index(compiled, index, &port)
                 : ov_compiled_model_output_by_index(compiled, index, &port),
          what, "port lookup");
    Handle<ov_output_const_port_t, ov_output_const_port_free> held(port, ov_output_const_port_free);
    char* name = nullptr;
    check(ov_port_get_any_name(port, &name), what, "port name");
    names.emplace_back(name);
    ov_free(name);
  }
  return names;
}

class OpenVinoModel final : public InferenceModel {
 public:
  explicit OpenVinoModel(const ModelSpec& spec) : what_(spec.what) {
    const OrtEpSelection selection = ort_ep_selection();
    ov_model_t* network = nullptr;
    check(ov_core_read_model(process_core(), spec.path.c_str(), nullptr, &network), what_,
          std::format("read of {}", spec.path.string()));
    Handle<ov_model_t, ov_model_free> network_handle(network, ov_model_free);
    // The GPU plugin picks half precision on its own; a model that says it
    // needs single precision gets it, as on the ONNX Runtime provider.
    std::vector<const char*> properties = {"PERFORMANCE_HINT", "LATENCY"};
    if (spec.precision == OrtPrecision::kFloat32) {
      properties.insert(properties.end(), {"INFERENCE_PRECISION_HINT", "f32"});
    }
    if (!selection.openvino_cache_dir.empty()) {
      properties.insert(properties.end(), {"CACHE_DIR", selection.openvino_cache_dir.c_str()});
    }
    ov_compiled_model_t* compiled = nullptr;
    {
      const OvCompileGate compile_gate;  // see grparse_session_ep.h
      const ov_status_e status = compile(network, selection.openvino_device, properties, &compiled);
      if (status != OK) {
        throw ep_refusal(what_, spec.path, std::format("OpenVINO {} compile: {}",
                                                       selection.openvino_device, ov_error(status)));
      }
    }
    compiled_.reset(compiled);
    ov_infer_request_t* request = nullptr;
    check(ov_compiled_model_create_infer_request(compiled_.get(), &request), what_, "infer request");
    request_.reset(request);
    input_names_ = port_names(compiled_.get(), true, what_);
    output_names_ = port_names(compiled_.get(), false, what_);
  }

  std::vector<Tensor> run(std::span<const TensorView> inputs) override {
    if (inputs.size() != input_names_.size()) {
      throw std::invalid_argument(std::format("{}: expected {} inputs, got {}", what_,
                                              input_names_.size(), inputs.size()));
    }
    // The views stay alive for this call, so the request reads them in place.
    std::vector<Handle<ov_tensor_t, ov_tensor_free>> held;
    for (size_t index = 0; index < inputs.size(); ++index) {
      const TensorView& view = inputs[index];
      ov_shape_t shape{};
      check(ov_shape_create(static_cast<int64_t>(view.shape.size()), view.shape.data(), &shape),
            what_, "shape");
      ov_tensor_t* tensor = nullptr;
      const ov_status_e status = ov_tensor_create_from_host_ptr(
          ov_element(view.element), shape, const_cast<void*>(view.data), &tensor);
      ov_shape_free(&shape);
      check(status, what_, "input tensor");
      held.emplace_back(tensor, ov_tensor_free);
      check(ov_infer_request_set_input_tensor_by_index(request_.get(), index, tensor), what_,
            "input binding");
    }
    check(ov_infer_request_infer(request_.get()), what_, "inference");
    std::vector<Tensor> outputs;
    outputs.reserve(output_names_.size());
    for (size_t index = 0; index < output_names_.size(); ++index) {
      ov_tensor_t* raw = nullptr;
      check(ov_infer_request_get_output_tensor_by_index(request_.get(), index, &raw), what_, "output");
      Handle<ov_tensor_t, ov_tensor_free> output(raw, ov_tensor_free);
      Tensor out;
      ov_element_type_e element = UNDEFINED;
      check(ov_tensor_get_element_type(output.get(), &element), what_, "output type");
      out.element = tensor_element(element, what_);
      ov_shape_t shape{};
      check(ov_tensor_get_shape(output.get(), &shape), what_, "output shape");
      out.shape.assign(shape.dims, shape.dims + shape.rank);
      ov_shape_free(&shape);
      size_t bytes = 0;
      check(ov_tensor_get_byte_size(output.get(), &bytes), what_, "output size");
      void* data = nullptr;
      check(ov_tensor_data(output.get(), &data), what_, "output data");
      out.bytes.resize(bytes);
      std::memcpy(out.bytes.data(), data, bytes);
      outputs.push_back(std::move(out));
    }
    return outputs;
  }

  // Static-shape graphs compile every kernel at compile time; nothing waits
  // for a first run.
  void prewarm(std::span<const std::vector<TensorSpec>>) override {}
  const std::vector<std::string>& input_names() const override { return input_names_; }
  const std::vector<std::string>& output_names() const override { return output_names_; }
  std::string_view backend_name() const override { return "OpenVINO"; }

 private:
  static ov_status_e compile(const ov_model_t* network, const std::string& device,
                             const std::vector<const char*>& p, ov_compiled_model_t** compiled) {
    ov_core_t* core = process_core();
    switch (p.size()) {
      case 2: return ov_core_compile_model(core, network, device.c_str(), 2, compiled, p[0], p[1]);
      case 4:
        return ov_core_compile_model(core, network, device.c_str(), 4, compiled, p[0], p[1], p[2], p[3]);
      case 6:
        return ov_core_compile_model(core, network, device.c_str(), 6, compiled, p[0], p[1], p[2],
                                     p[3], p[4], p[5]);
      default: throw std::logic_error("OpenVINO compile: unexpected property count");
    }
  }

  std::string what_;
  Handle<ov_compiled_model_t, ov_compiled_model_free> compiled_{nullptr, ov_compiled_model_free};
  Handle<ov_infer_request_t, ov_infer_request_free> request_{nullptr, ov_infer_request_free};
  std::vector<std::string> input_names_;
  std::vector<std::string> output_names_;
};

}  // namespace

bool native_openvino_build() { return true; }

std::unique_ptr<InferenceModel> make_openvino_model(const ModelSpec& spec) {
  auto model = std::make_unique<OpenVinoModel>(spec);
  record_native_placement(spec.what);  // reached only when the model compiled
  return model;
}

}  // namespace grparse
