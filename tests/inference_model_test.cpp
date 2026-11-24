// Proves the typed model interface (compat/grparse_inference_model.h): an
// ONNX Runtime session behind it runs and copies its outputs typed, a device
// failure maps to UNAVAILABLE, and the shape policy (optimization profiles,
// line width limits) exists exactly where native TensorRT runs the nets.

#include <climits>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
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
      std::make_exception_ptr(grparse::InferenceDeviceError("TensorRT device failed layout")));
  require(status.error_code() == grpc::StatusCode::UNAVAILABLE,
          "a device failure is retryable: UNAVAILABLE");
}

// A profile that serves one shape per input.
void verify_static_profile() {
  const grparse::ShapeProfile profile = grparse::static_profile({{1, 3, 640, 640}, {1, 2}});
  require_equal(profile.inputs.size(), size_t{2}, "one range per input");
  require(profile.inputs[0].min == profile.inputs[0].max &&
              profile.inputs[0].opt == std::vector<int64_t>({1, 3, 640, 640}),
          "min, opt and max are the one shape");
  require(profile.inputs[1].opt == std::vector<int64_t>({1, 2}), "the second input keeps its shape");
}

// ONNX Runtime takes shapes as they come: no profiles, no prewarm, no line
// width limits.  A CUDA selection has them only in a build that carries
// native TensorRT.
void verify_shape_policy_follows_backend() {
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCpu;
  grparse::set_ort_ep_selection(selection);
  for (const grparse::OcrNet net : {grparse::OcrNet::kDetection, grparse::OcrNet::kDirection,
                                    grparse::OcrNet::kRecognition}) {
    require(grparse::ocr_net_shapes(net).profiles.empty(), "CPU declares no profiles");
    require(grparse::ocr_prewarm_sets(net).empty(), "CPU has nothing to prewarm");
  }
  require_equal(grparse::ocr_rec_min_width(), 1, "CPU reads any narrow line");
  require_equal(grparse::ocr_rec_max_width(), INT_MAX, "CPU reads any wide line");

  selection.ep = grparse::OrtEp::kCuda;
  grparse::set_ort_ep_selection(selection);
  const bool native = grparse::native_tensorrt_build();
  require_equal(grparse::ocr_net_shapes(grparse::OcrNet::kDetection).profiles.empty(), !native,
                "the detector has profiles exactly under native TensorRT");
  require_equal(grparse::ocr_rec_max_width() == INT_MAX, !native,
                "the recognizer width is capped exactly under native TensorRT");
  selection.ep = grparse::OrtEp::kCpu;
  grparse::set_ort_ep_selection(selection);
}

// Without TensorRT in the build no CUDA device is ever reported present, so
// GRPARSE_ORT_EP=auto cannot pick a backend the binary lacks.
void verify_device_probe_without_tensorrt() {
  if (grparse::native_tensorrt_build()) return;
  std::string detail;
  require(!grparse::tensorrt_device_present(0, &detail), "no TensorRT, no device");
  require(detail.find("no TensorRT") != std::string::npos, "the detail says why");
}

// A model that keeps what run() was handed.
class RecordingModel final : public grparse::InferenceModel {
 public:
  std::vector<grparse::Tensor> run(std::span<const grparse::TensorView> inputs) override {
    shape = inputs[0].shape;
    const auto* floats = static_cast<const float*>(inputs[0].data);
    values.assign(floats, floats + shape[0] * shape[1] * shape[2] * shape[3]);
    return {};
  }
  void prewarm(std::span<const std::vector<grparse::TensorSpec>>) override {}
  const std::vector<std::string>& input_names() const override { return names; }
  const std::vector<std::string>& output_names() const override { return names; }
  std::string_view backend_name() const override { return "recording"; }

  std::vector<int64_t> shape;
  std::vector<float> values;
  std::vector<std::string> names{"x"};
};

// A backend without device normalization gets the tensor RapidOCR's
// substractMeanNormalize builds from the same bytes, bit for bit, image by
// image.
void verify_run_image_normalizes_on_the_host() {
  const int count = 2, height = 2, width = 3;
  uint8_t pixels[count * height * width * 3];
  for (size_t i = 0; i < sizeof(pixels); ++i) pixels[i] = static_cast<uint8_t>(i * 37 + (i == 5 ? 255 : 0));
  const float mean[3] = {0.485F * 255, 0.456F * 255, 0.406F * 255};
  const float norm[3] = {1.0F / 0.229F / 255.0F, 1.0F / 0.224F / 255.0F, 1.0F / 0.225F / 255.0F};
  RecordingModel model;
  model.run_image(pixels, count, height, width, mean, norm);
  require(model.shape == std::vector<int64_t>({count, 3, height, width}), "the input is [count, 3, height, width]");
  const size_t plane = size_t{height} * width;
  for (size_t image = 0; image < count; ++image) {
    for (size_t pid = 0; pid < plane; ++pid) {
      for (size_t ch = 0; ch < 3; ++ch) {
        const float expected = (float)(pixels[(image * plane + pid) * 3 + ch] * norm[ch] - mean[ch] * norm[ch]);
        require(model.values[(image * 3 + ch) * plane + pid] == expected,
                std::format("image {} pixel {} channel {} is normalized as RapidOCR does", image, pid, ch));
      }
    }
  }
}

}  // namespace

int main() {
  return grparse_test::run_test_main("inference-model-test", "all checks passed", {
      verify_ort_model_runs_typed,
      verify_device_error_is_unavailable,
      verify_static_profile,
      verify_shape_policy_follows_backend,
      verify_device_probe_without_tensorrt,
      verify_run_image_normalizes_on_the_host,
  });
}
