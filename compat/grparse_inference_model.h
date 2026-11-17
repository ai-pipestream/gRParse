#pragma once

// One model, one typed run call, whatever runs it.  The cpu and cuda flavors
// back it with an ONNX Runtime session (exactly the session make_session and
// make_rapidocr_session build); the openvino flavor backs it with native
// OpenVINO: one compiled model per static input shape, an infer-request pool
// per shape, and no CPU device anywhere.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "grparse_session_ep.h"

namespace grparse {

enum class TensorElement { kFloat32, kInt64, kUInt8 };

size_t tensor_element_size(TensorElement element);

// An input's element type and static shape, without data (prewarm sets).
struct TensorSpec {
  TensorElement element = TensorElement::kFloat32;
  std::vector<int64_t> shape;
};

// A borrowed input: the caller keeps `data` alive for the run call.
struct TensorView {
  TensorElement element = TensorElement::kFloat32;
  const void* data = nullptr;
  std::vector<int64_t> shape;
};

// An owned output, copied off the device.
struct Tensor {
  TensorElement element = TensorElement::kFloat32;
  std::vector<int64_t> shape;
  std::vector<std::byte> bytes;

  size_t element_count() const;
  const float* floats() const;
  const int64_t* int64s() const;
};

// Thrown when the device fails an inference (an OpenVINO GPU reset, a lost
// context): the request fails with UNAVAILABLE naming the device, and the
// model drops every compiled shape so the next call rebuilds them.
class InferenceDeviceError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// How a model's batch dimension is fixed on a provider that compiles per
// shape: a call with n rows runs at the smallest ladder entry >= n, padded
// with zero rows, and only the first n output rows come back.  Empty leaves
// the batch dimension as the caller sends it.
struct ModelShapePolicy {
  std::vector<int64_t> batch_ladder;
};

struct ModelSpec {
  std::filesystem::path path;
  // Names the model in logs, errors and metrics ("layout", "OCR detection").
  std::string_view what;
  OrtPrecision precision = OrtPrecision::kProviderDefault;
  int intra_op_threads = kIntraOpProcessDefault;
  ModelShapePolicy shapes;
};

class InferenceModel {
 public:
  virtual ~InferenceModel() = default;
  // Inputs in the model's input order; outputs in its output order.
  virtual std::vector<Tensor> run(std::span<const TensorView> inputs) = 0;
  // Compiles (or loads from the kernel cache) and runs once every listed
  // input set, so no compile happens under load; a shape compiled later is
  // counted (openvino_runtime_compile_count).  A no-op for ONNX Runtime.
  virtual void prewarm(std::span<const std::vector<TensorSpec>> input_sets) = 0;
  virtual const std::vector<std::string>& input_names() const = 0;
  virtual const std::vector<std::string>& output_names() const = 0;
  size_t input_count() const { return input_names().size(); }
  size_t output_count() const { return output_names().size(); }
  virtual std::string_view backend_name() const = 0;
};

// Builds the model on the process selection (set_ort_ep_selection):
// native OpenVINO when the selection is OpenVINO, an ONNX Runtime session
// (make_session's policy) otherwise.  Native OpenVINO never falls back to
// CPU: a model the device will not compile throws, naming it.
std::unique_ptr<InferenceModel> make_inference_model(const ModelSpec& spec);

// An ONNX Runtime session behind the interface (make_cpu_session's result,
// for the ORT flavors' existing CPU retreat).
std::unique_ptr<InferenceModel> wrap_ort_session(Ort::Session session);

// The three RapidOCR nets (patches/rapidocr-model-backend.patch).
enum class OcrNet { kDetection, kDirection, kRecognition };

std::string_view ocr_net_name(OcrNet net);

// How a net's shapes are fixed under the process selection: on native
// OpenVINO the direction and recognition nets run fixed batches (see
// ocr_batch_size), every other provider leaves shapes as they come.
ModelShapePolicy ocr_net_shapes(OcrNet net);

// Every input set a net can see once its shapes are bucketed (detector
// ladder squared, recognizer width ladder at the fixed batch, the direction
// net's one shape), for prewarm at startup.  Empty when nothing is bucketed.
std::vector<std::vector<TensorSpec>> ocr_prewarm_sets(OcrNet net);

// The ONNX Runtime-backed model the RapidOCR nets used before (built through
// make_rapidocr_session's retry policy), or native OpenVINO under an
// OpenVINO selection.
std::unique_ptr<InferenceModel> make_rapidocr_model(Ort::Env& env, const std::filesystem::path& path,
                                                    OcrNet net, int num_thread);

// GRPARSE_OPENVINO_ISOLATION=process (default) runs native OpenVINO in a
// supervised child process (src/inference_host.cpp) so a device loss or a
// crash inside the GPU stack fails calls with UNAVAILABLE instead of taking
// the server down; =none runs it in the server process.
bool openvino_process_isolation();

// Runs the inference host when argv is `--grparse-inference-host <fd>`
// (main() calls it first) and never returns in that case; false otherwise.
bool maybe_run_inference_host(int argc, char** argv);

// Inference host restarts since start (grparse_openvino_host_restarts_total).
uint64_t openvino_host_restart_count();

// Device failures counted since start (grparse_openvino_device_errors_total)
// and shapes compiled after startup prewarm (grparse_openvino_runtime_compiles_total).
uint64_t openvino_device_error_count();
uint64_t openvino_runtime_compile_count();

}  // namespace grparse
