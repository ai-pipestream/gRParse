// Proves the typed model interface (compat/grparse_inference_model.h): an
// ONNX Runtime session behind it runs and copies its outputs typed, a device
// failure maps to UNAVAILABLE, and on a native OpenVINO build the shape
// policy fixes the detector to its twelve letterbox buckets and the line nets
// to batches of eight.

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <span>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "grparse_inference_model.h"
#include "grparse_session_ep.h"
#include "../src/parse_support.h"
#include "support/check.h"

namespace {

using grparse_test::require;
using grparse_test::require_equal;

// One Identity from float input "x" [1] to output "y" [1] (the same fixture
// ort_session_ep_test uses).
constexpr unsigned char kTinyModelBytes[] = {
    0x08, 0x08, 0x42, 0x02, 0x10, 0x0D, 0x3A, 0x37, 0x12, 0x01, 0x67, 0x0A, 0x10, 0x0A, 0x01,
    0x78, 0x12, 0x01, 0x79, 0x22, 0x08, 0x49, 0x64, 0x65, 0x6E, 0x74, 0x69, 0x74, 0x79, 0x5A,
    0x0F, 0x0A, 0x01, 0x78, 0x12, 0x0A, 0x0A, 0x08, 0x08, 0x01, 0x12, 0x04, 0x0A, 0x02, 0x08,
    0x01, 0x62, 0x0F, 0x0A, 0x01, 0x79, 0x12, 0x0A, 0x0A, 0x08, 0x08, 0x01, 0x12, 0x04, 0x0A,
    0x02, 0x08, 0x01,
};

std::filesystem::path write_tiny_model() {
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("grparse-inference-tiny-{}.onnx", ::getpid());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(kTinyModelBytes), sizeof(kTinyModelBytes));
  out.close();
  require(out.good(), "the tiny model fixture is written");
  return path;
}

void verify_ort_model_runs_typed() {
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCpu;
  grparse::set_ort_ep_selection(selection);
  const auto path = write_tiny_model();
  auto model = grparse::make_inference_model(grparse::ModelSpec{
      path, "tiny", grparse::OrtPrecision::kProviderDefault, grparse::kIntraOpProcessDefault, {}});
  require(model->backend_name() == "onnxruntime", "a CPU selection runs on ONNX Runtime");
  require(model->input_names() == std::vector<std::string>{"x"}, "the input is named x");
  require(model->output_names() == std::vector<std::string>{"y"}, "the output is named y");
  const float value = 2.5F;
  const grparse::TensorView input{grparse::TensorElement::kFloat32, &value, {1}};
  const auto outputs = model->run(std::span(&input, 1));
  require_equal(outputs.size(), size_t{1}, "one output");
  require(outputs[0].shape == std::vector<int64_t>{1}, "the output shape is [1]");
  require(outputs[0].floats()[0] == 2.5F, "Identity returns its input");
  model->prewarm({});  // a no-op on ONNX Runtime
  std::filesystem::remove(path);
}

void verify_device_error_is_unavailable() {
  const grpc::Status status = grparse::status_from_exception(
      std::make_exception_ptr(grparse::InferenceDeviceError("OpenVINO GPU device failed layout")));
  require(status.error_code() == grpc::StatusCode::UNAVAILABLE,
          "a device failure is retryable: UNAVAILABLE");
}

void verify_native_shape_policy() {
  if (!grparse::native_openvino_build()) return;
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kOpenVino;
  selection.openvino_device = "GPU";
  grparse::set_ort_ep_selection(selection);
  require(grparse::ocr_det_max_side(2048) == 1600, "the detector is capped at 1600");
  require(grparse::ocr_det_max_side(960) == 960, "a smaller configured side stays");
  // Every resize the detector can see lands in one of the eight buckets,
  // and every bucket covers its input.
  std::set<std::pair<int, int>> buckets;
  for (int width = 32; width <= 1600; width += 32) {
    for (int height = 32; height <= 1600; height += 32) {
      const auto [bucket_width, bucket_height] = grparse::ocr_det_bucket_shape(width, height);
      require(bucket_width >= width && bucket_height >= height, "a bucket covers its input");
      buckets.insert({bucket_width, bucket_height});
    }
  }
  const auto prewarm = grparse::ocr_prewarm_sets(grparse::OcrNet::kDetection);
  std::set<std::pair<int, int>> warmed;
  for (const auto& set : prewarm) warmed.insert({static_cast<int>(set[0].shape[3]), static_cast<int>(set[0].shape[2])});
  for (const auto& bucket : buckets) {
    require(warmed.contains(bucket), std::format("bucket {}x{} is prewarmed", bucket.first, bucket.second));
  }
  require_equal(buckets.size(), size_t{12}, "every detector input lands in one of twelve buckets");
  require_equal(prewarm.size(), size_t{12}, "the twelve detector buckets are prewarmed");
  require(grparse::ocr_net_shapes(grparse::OcrNet::kRecognition).batch_ladder == std::vector<int64_t>{8},
          "recognition runs batches of eight");
  require(grparse::ocr_net_shapes(grparse::OcrNet::kDetection).batch_ladder.empty(),
          "detection runs one page at a time");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("inference-model-test", "all checks passed", {
      verify_ort_model_runs_typed,
      verify_device_error_is_unavailable,
      verify_native_shape_policy,
  });
}
