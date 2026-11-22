#pragma once

// One model, one typed run call, whatever runs it.  The cpu flavor backs it
// with an ONNX Runtime session (exactly the session make_session and
// make_rapidocr_session build); the cuda flavor backs it with native TensorRT
// (src/tensorrt/): one engine per model, built at startup with explicit
// optimization profiles, and no ONNX Runtime session anywhere.

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

// Thrown when the device fails an inference (a lost CUDA context, a failed
// enqueue): the request fails with UNAVAILABLE naming the device.
class InferenceDeviceError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The shapes one input takes inside one optimization profile: the smallest,
// the one the builder tunes its kernels for, and the largest.
struct ShapeRange {
  std::vector<int64_t> min;
  std::vector<int64_t> opt;
  std::vector<int64_t> max;
};

// One optimization profile: a range per input, in the model's input order.
struct ShapeProfile {
  std::vector<ShapeRange> inputs;
};

// A profile that serves exactly one shape per input.
ShapeProfile static_profile(std::vector<std::vector<int64_t>> shapes);

// How a model's shapes are fixed on a backend that compiles for them.
struct ModelShapePolicy {
  // A call with n rows runs at the smallest ladder entry >= n, padded with
  // zero rows, and only the first n output rows come back.  Empty leaves the
  // batch dimension as the caller sends it.  Unused by native TensorRT.
  std::vector<int64_t> batch_ladder;
  // Native TensorRT: the optimization profiles the engine is built with.  A
  // run uses the covering profile with the smallest largest-shape; rows past
  // that profile's largest batch run in several calls.  A model whose inputs
  // are all static needs none; a shape no profile covers fails the call.
  // Ignored by ONNX Runtime.
  std::vector<ShapeProfile> profiles;
};

struct ModelSpec {
  std::filesystem::path path;
  // Names the model in logs, errors and metrics ("layout", "OCR detection").
  std::string_view what;
  // What ONNX Runtime's provider is asked for.  Native TensorRT has no such
  // choice to make: an engine computes in the types the model file declares
  // (FP32 for every file in models/MANIFEST), with TF32 off.
  OrtPrecision precision = OrtPrecision::kProviderDefault;
  int intra_op_threads = kIntraOpProcessDefault;
  ModelShapePolicy shapes;
};

class InferenceModel {
 public:
  virtual ~InferenceModel() = default;
  // Inputs in the model's input order; outputs in its output order.
  virtual std::vector<Tensor> run(std::span<const TensorView> inputs) = 0;
  // Runs once every listed input set, so no first-use setup happens under
  // load.  A no-op for ONNX Runtime.
  virtual void prewarm(std::span<const std::vector<TensorSpec>> input_sets) = 0;
  virtual const std::vector<std::string>& input_names() const = 0;
  virtual const std::vector<std::string>& output_names() const = 0;
  size_t input_count() const { return input_names().size(); }
  size_t output_count() const { return output_names().size(); }
  virtual std::string_view backend_name() const = 0;
};

// Builds the model on the process selection (set_ort_ep_selection): native
// TensorRT when the selection is CUDA in a build that carries TensorRT
// (native_tensorrt_build), an ONNX Runtime session (make_session's policy)
// otherwise.  Native TensorRT never falls back: a model that will not build
// as an engine throws ep_refusal's error with the builder's message, and no
// ONNX Runtime session is built in its place.
std::unique_ptr<InferenceModel> make_inference_model(const ModelSpec& spec);

// Whether CUDA device `cuda_device` exists and initializes, in a build that
// carries native TensorRT; `detail` gets CUDA's own words when it does not.
// Always false, with a detail saying so, in a build without TensorRT.
bool tensorrt_device_present(int cuda_device, std::string* detail);

// An ONNX Runtime session behind the interface (make_cpu_session's result,
// for the ORT flavors' existing CPU retreat).
std::unique_ptr<InferenceModel> wrap_ort_session(Ort::Session session);

// The three RapidOCR nets (patches/rapidocr-model-backend.patch).
enum class OcrNet { kDetection, kDirection, kRecognition };

std::string_view ocr_net_name(OcrNet net);

// How a net's shapes are fixed under the process selection: on native
// TensorRT the optimization profiles of src/tensorrt/ocr_profiles.cpp; ONNX
// Runtime leaves shapes as they come.
ModelShapePolicy ocr_net_shapes(OcrNet net);

// Every input set a net is warmed with at startup; empty when the backend
// has nothing to warm (ONNX Runtime).
std::vector<std::vector<TensorSpec>> ocr_prewarm_sets(OcrNet net);

// The ONNX Runtime-backed model the RapidOCR nets used before (built through
// make_rapidocr_session's retry policy), or native TensorRT under a CUDA
// selection in a TensorRT build.
std::unique_ptr<InferenceModel> make_rapidocr_model(Ort::Env& env, const std::filesystem::path& path,
                                                    OcrNet net, int num_thread);

// The narrowest and widest line (pixels at height 48) the recognition net
// reads: a narrower crop is padded with zeros, as a batch pads its short
// lines, and a wider one is resized down to the limit
// (patches/rapidocr-rec-width-limits.patch).  1 and INT_MAX on ONNX Runtime,
// which takes any width; on native TensorRT the bounds of its profiles
// (src/tensorrt/ocr_profiles.h).
int ocr_rec_min_width();
int ocr_rec_max_width();

// The detector's largest input side follows GRPARSE_OCR_MAX_SIDE and
// GRPARSE_OCR_PADDING; the OCR engine reports both before its nets
// initialize, so the detector's TensorRT profiles cover every input the
// configuration can produce.
void set_ocr_detector_side_limit(int max_side_len, int padding);

}  // namespace grparse
