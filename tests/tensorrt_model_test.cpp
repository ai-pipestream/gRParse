// The GPU check of the native TensorRT backend (src/tensorrt/), shipped in
// the CUDA image as grparse-tensorrt-check and run by the krick GPU suite
// (scripts/test/gpu-placement-cuda-test.sh):
//
//   grparse-tensorrt-check            models from GRPARSE_MODELS_DIR (/models)
//   GRPARSE_TENSORRT_CHECK_IMAGE=<png>  a real page to feed the models
//
// Every model file is built as a TensorRT engine and compared, on the same
// input, with the ONNX Runtime CPU session of the same file: the reference
// the goldens were recorded against.  Also: rows past a profile's batch,
// concurrent runs, the engine cache, and a model TensorRT cannot import.
//
// Needs an NVIDIA GPU and the model files.  Without a GPU it skips (77), or
// fails under GRPARSE_TEST_REQUIRE_GPU=1; without the models it skips, or
// fails under GRPARSE_TEST_REQUIRE_MODELS=1.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "../src/tensorrt/tensorrt_model.h"
#include "grparse_inference_model.h"
#include "grparse_session_ep.h"
#include "support/check.h"

namespace {

using grparse::InferenceModel;
using grparse::ModelSpec;
using grparse::Tensor;
using grparse::TensorElement;
using grparse::TensorView;
using grparse_test::require;
using grparse_test::require_equal;

std::filesystem::path models_dir() {
  const char* configured = std::getenv("GRPARSE_MODELS_DIR");
  return configured == nullptr ? "/models" : configured;
}

// A page to read: the real one the suite passes, or a drawn one with lines
// of text, a ruled table and a filled figure, so no model sees noise.
cv::Mat test_page() {
  if (const char* path = std::getenv("GRPARSE_TENSORRT_CHECK_IMAGE"); path != nullptr) {
    cv::Mat image = cv::imread(path, cv::IMREAD_COLOR);
    require(!image.empty(), std::format("GRPARSE_TENSORRT_CHECK_IMAGE={} is not an image", path));
    return image;
  }
  cv::Mat page(2200, 1700, CV_8UC3, cv::Scalar(255, 255, 255));
  for (int line = 0; line < 30; ++line) {
    cv::putText(page, std::format("Line {} of the page reads 0123456789 and so on.", line),
                cv::Point(120, 160 + line * 44), cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 0), 2);
  }
  for (int rule = 0; rule <= 4; ++rule) {
    cv::line(page, cv::Point(120, 1560 + rule * 60), cv::Point(1580, 1560 + rule * 60), cv::Scalar(0, 0, 0), 2);
    cv::line(page, cv::Point(120 + rule * 365, 1560), cv::Point(120 + rule * 365, 1800), cv::Scalar(0, 0, 0), 2);
  }
  cv::rectangle(page, cv::Rect(900, 1850, 600, 280), cv::Scalar(40, 90, 200), cv::FILLED);
  return page;
}

// BGR page -> RGB float NCHW, (pixel / 255 - mean) / std, at width x height.
std::vector<float> normalized(const cv::Mat& image, int width, int height, float mean, float std) {
  cv::Mat resized;
  cv::resize(image, resized, cv::Size(width, height));
  std::vector<float> tensor(static_cast<size_t>(3) * width * height);
  const size_t plane = static_cast<size_t>(width) * height;
  for (int row = 0; row < height; ++row) {
    const cv::Vec3b* pixels = resized.ptr<cv::Vec3b>(row);
    for (int column = 0; column < width; ++column) {
      for (int channel = 0; channel < 3; ++channel) {
        tensor[plane * channel + static_cast<size_t>(row) * width + column] =
            (static_cast<float>(pixels[column][2 - channel]) / 255.0F - mean) / std;
      }
    }
  }
  return tensor;
}

std::unique_ptr<InferenceModel> reference_model(const std::filesystem::path& path) {
  static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-tensorrt-check");
  return grparse::wrap_ort_session(grparse::make_cpu_session(env, path, grparse::kIntraOpAllCores));
}

double max_abs_difference(const Tensor& actual, const Tensor& expected, std::string_view what) {
  require(actual.shape == expected.shape, std::format("{}: output shapes agree", what));
  require(actual.element == expected.element, std::format("{}: output types agree", what));
  double worst = 0.0;
  const size_t count = actual.element_count();
  if (actual.element == TensorElement::kInt64) {
    for (size_t index = 0; index < count; ++index) {
      worst = std::max(worst, std::abs(static_cast<double>(actual.int64s()[index] - expected.int64s()[index])));
    }
    return worst;
  }
  for (size_t index = 0; index < count; ++index) {
    worst = std::max(worst, std::abs(static_cast<double>(actual.floats()[index]) - expected.floats()[index]));
  }
  return worst;
}

// Runs both backends on `inputs` and holds every output to `tolerance`.
std::vector<Tensor> compare(InferenceModel& engine, InferenceModel& reference,
                            std::span<const TensorView> inputs, double tolerance,
                            std::string_view what) {
  require(engine.backend_name() == "tensorrt", std::format("{} runs on TensorRT", what));
  require(engine.input_names() == reference.input_names() &&
              engine.output_names() == reference.output_names(),
          std::format("{}: the engine keeps the model's input and output names", what));
  std::vector<Tensor> actual = engine.run(inputs);
  const std::vector<Tensor> expected = reference.run(inputs);
  require_equal(actual.size(), expected.size(), std::format("{}: output count", what));
  for (size_t index = 0; index < actual.size(); ++index) {
    const double worst = max_abs_difference(actual[index], expected[index], what);
    std::println("  {} output {}: max difference {:.2e} (allowed {:.0e})", what, index, worst, tolerance);
    require(worst <= tolerance,
            std::format("{} output {} differs from the CPU reference by {:.3e}", what, index, worst));
  }
  return actual;
}

std::unique_ptr<InferenceModel> engine_for(const std::filesystem::path& path, std::string_view what,
                                           grparse::ModelShapePolicy shapes = {}) {
  return grparse::make_inference_model(ModelSpec{path, what, grparse::OrtPrecision::kFloat32,
                                                 grparse::kIntraOpProcessDefault, std::move(shapes)});
}

void verify_ocr_nets(const cv::Mat& page) {
  const auto directory = models_dir();
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-tensorrt-check-ocr");
  // Detection at a small picture, an odd page and a full letter page.
  const auto det_path = directory / "ch_PP-OCRv3_det_infer.onnx";
  auto det = grparse::make_rapidocr_model(env, det_path, grparse::OcrNet::kDetection, 1);
  auto det_reference = reference_model(det_path);
  for (const auto& [width, height] : {std::pair{640, 480}, std::pair{1056, 1376}, std::pair{1664, 2144}}) {
    const std::vector<float> tensor = normalized(page, width, height, 0.45F, 0.225F);
    const TensorView view{TensorElement::kFloat32, tensor.data(), {1, 3, height, width}};
    compare(*det, *det_reference, std::span(&view, 1), 1e-2, std::format("OCR detection {}x{}", width, height));
  }
  // Recognition: forty strips of the page, more rows than one call carries.
  const auto rec_path = directory / "ch_PP-OCRv3_rec_infer.onnx";
  auto rec = grparse::make_rapidocr_model(env, rec_path, grparse::OcrNet::kRecognition, 1);
  auto rec_reference = reference_model(rec_path);
  constexpr int kRows = 40;
  constexpr int kWidth = 600;
  std::vector<float> lines;
  for (int row = 0; row < kRows; ++row) {
    const cv::Rect strip(0, std::min(page.rows - 48, 100 + row * 40), page.cols, 48);
    const std::vector<float> line = normalized(page(strip), kWidth, 48, 0.5F, 0.5F);
    lines.insert(lines.end(), line.begin(), line.end());
  }
  const TensorView batch{TensorElement::kFloat32, lines.data(), {kRows, 3, 48, kWidth}};
  const std::vector<Tensor> read = compare(*rec, *rec_reference, std::span(&batch, 1), 2e-3,
                                           "OCR recognition 40x600");
  require_equal(read[0].shape.front(), int64_t{kRows}, "a split run returns every row");
  for (const int width : {16, 64, 77, 2560, 4000}) {
    const std::vector<float> line = normalized(page, width, 48, 0.5F, 0.5F);
    const TensorView view{TensorElement::kFloat32, line.data(), {1, 3, 48, width}};
    compare(*rec, *rec_reference, std::span(&view, 1), 2e-3, std::format("OCR recognition 1x{}", width));
  }
  const auto cls_path = directory / "ch_ppocr_mobile_v2.0_cls_infer.onnx";
  auto cls = grparse::make_rapidocr_model(env, cls_path, grparse::OcrNet::kDirection, 1);
  auto cls_reference = reference_model(cls_path);
  const std::vector<float> crops(lines.begin(), lines.begin() + 16 * 3 * 48 * 192);
  const TensorView crop_view{TensorElement::kFloat32, crops.data(), {16, 3, 48, 192}};
  compare(*cls, *cls_reference, std::span(&crop_view, 1), 2e-3, "OCR direction 16x192");
}

// The query detector: every detection the CPU reference keeps, from one
// thread and from several at once.
void verify_layout(const cv::Mat& page) {
  const auto path = models_dir() / "layout_heron.onnx";
  if (!std::filesystem::exists(path)) {
    std::println("  layout_heron.onnx absent; skipped");
    return;
  }
  grparse::ModelShapePolicy shapes;
  shapes.profiles.push_back(grparse::static_profile({{1, 3, 640, 640}, {1, 2}}));
  auto engine = engine_for(path, "layout", shapes);
  auto reference = reference_model(path);
  cv::Mat resized;
  cv::resize(page, resized, cv::Size(640, 640));
  std::vector<uint8_t> pixels(static_cast<size_t>(3) * 640 * 640);
  for (int row = 0; row < 640; ++row) {
    for (int column = 0; column < 640; ++column) {
      for (int channel = 0; channel < 3; ++channel) {
        pixels[static_cast<size_t>(channel) * 640 * 640 + static_cast<size_t>(row) * 640 + column] =
            resized.at<cv::Vec3b>(row, column)[2 - channel];
      }
    }
  }
  const std::vector<int64_t> size = {page.cols, page.rows};
  const std::vector<TensorView> inputs = {
      TensorView{TensorElement::kUInt8, pixels.data(), {1, 3, 640, 640}},
      TensorView{TensorElement::kInt64, size.data(), {1, 2}}};
  const std::vector<Tensor> expected = reference->run(inputs);
  // The outputs are the 300 best queries in score order, so two queries with
  // nearly equal scores may trade places and a query nobody would keep may
  // flip its label.  What must agree is every detection the decode can keep:
  // each reference query scoring 0.2 or more has an engine query with the
  // same label, a score within 2e-3 and a box within half a pixel.
  const auto check = [&](const std::vector<Tensor>& actual) {
    require(actual[0].shape == expected[0].shape && actual[1].shape == expected[1].shape,
            "layout output shapes agree");
    const auto queries = static_cast<size_t>(expected[2].shape[1]);
    double worst_box = 0.0;
    double worst_score = 0.0;
    size_t kept = 0;
    for (size_t want = 0; want < queries; ++want) {
      if (expected[2].floats()[want] < 0.2F) continue;
      ++kept;
      double best_box = 1e9;
      double best_score = 1e9;
      for (size_t got = 0; got < queries; ++got) {
        if (actual[0].int64s()[got] != expected[0].int64s()[want]) continue;
        double box = 0.0;
        for (size_t side = 0; side < 4; ++side) {
          box = std::max(box, std::abs(static_cast<double>(actual[1].floats()[got * 4 + side]) -
                                       expected[1].floats()[want * 4 + side]));
        }
        if (box >= best_box) continue;
        best_box = box;
        best_score = std::abs(static_cast<double>(actual[2].floats()[got]) - expected[2].floats()[want]);
      }
      require(best_box <= 0.5 && best_score <= 2e-3,
              std::format("layout detection {} (score {:.3f}) has no engine match: nearest box {:.3f} px, "
                          "score off by {:.2e}",
                          want, expected[2].floats()[want], best_box, best_score));
      worst_box = std::max(worst_box, best_box);
      worst_score = std::max(worst_score, best_score);
    }
    require(kept > 0, "the page has layout detections to compare");
    return std::tuple{kept, worst_box, worst_score};
  };
  const auto [kept, boxes, scores] = check(engine->run(inputs));
  std::println("  layout: {} detections agree, boxes within {:.3f} px, scores within {:.2e}", kept, boxes,
               scores);
  std::vector<std::thread> threads;
  std::vector<std::string> failures(4);
  for (size_t worker = 0; worker < failures.size(); ++worker) {
    threads.emplace_back([&, worker] {
      try {
        for (int round = 0; round < 8; ++round) check(engine->run(inputs));
      } catch (const std::exception& error) {
        failures[worker] = error.what();
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  for (const std::string& failure : failures) require(failure.empty(), "concurrent layout run: " + failure);
}

void verify_fixed_shape_models(const cv::Mat& page) {
  const auto directory = models_dir();
  const auto run_image = [&](const char* file, std::string_view what, int width, int height,
                             grparse::ModelShapePolicy shapes, double tolerance) {
    const auto path = directory / file;
    if (!std::filesystem::exists(path)) {
      std::println("  {} absent; skipped", file);
      return;
    }
    auto engine = engine_for(path, what, std::move(shapes));
    auto reference = reference_model(path);
    const std::vector<float> tensor = normalized(page, width, height, 0.45F, 0.225F);
    const TensorView view{TensorElement::kFloat32, tensor.data(), {1, 3, height, width}};
    compare(*engine, *reference, std::span(&view, 1), tolerance, what);
  };
  grparse::ModelShapePolicy one_crop;
  one_crop.profiles.push_back(grparse::static_profile({{1, 3, 224, 224}}));
  run_image("figure_classifier.onnx", "figure classes", 224, 224, one_crop, 2e-3);
  run_image("layout_publaynet.onnx", "layout (anchor-free)", 608, 800, {}, 2e-3);
  run_image("slanet_plus_encoder.onnx", "table structure encoder", 488, 488, {}, 2e-3);
  const auto step_path = directory / "slanet_plus_step.onnx";
  if (!std::filesystem::exists(step_path)) return;
  auto step = engine_for(step_path, "table structure decoder");
  auto reference = reference_model(step_path);
  std::vector<float> features(256 * 96);
  for (size_t index = 0; index < features.size(); ++index) features[index] = std::sin(0.37F * static_cast<float>(index));
  std::vector<float> hidden(256, 0.0F);
  std::vector<float> previous(50, 0.0F);
  previous[0] = 1.0F;
  // Twenty steps of feedback through the engine, each held to the reference.
  for (int index = 0; index < 20; ++index) {
    const std::vector<TensorView> inputs = {
        TensorView{TensorElement::kFloat32, features.data(), {1, 256, 96}},
        TensorView{TensorElement::kFloat32, hidden.data(), {1, 256}},
        TensorView{TensorElement::kFloat32, previous.data(), {1, 50}}};
    const std::vector<Tensor> actual = step->run(inputs);
    const std::vector<Tensor> expected = reference->run(inputs);
    for (size_t output = 0; output < actual.size(); ++output) {
      require(max_abs_difference(actual[output], expected[output], "table structure decoder") <= 1e-4,
              std::format("table structure decoder step {} output {} matches the reference", index, output));
    }
    hidden.assign(actual[0].floats(), actual[0].floats() + 256);
    std::ranges::fill(previous, 0.0F);
    previous[static_cast<size_t>(index % 50)] = 1.0F;
  }
  std::println("  table structure decoder: 20 fed-back steps within 1e-4");
}

// A second process start with the same model: the engine comes from the
// cache directory, not from a build.  Stood in for by the same bytes under
// another path, which misses the process registry and hits the cache.
void verify_engine_cache() {
  const auto source = models_dir() / "ch_ppocr_mobile_v2.0_cls_infer.onnx";
  const auto scratch = std::filesystem::temp_directory_path() / std::format("grparse-trt-check-{}", ::getpid());
  std::filesystem::remove_all(scratch);
  std::filesystem::create_directories(scratch / "a");
  std::filesystem::create_directories(scratch / "b");
  std::filesystem::copy_file(source, scratch / "a" / source.filename());
  std::filesystem::copy_file(source, scratch / "b" / source.filename());
  ::setenv("GRPARSE_TENSORRT_CACHE_DIR", (scratch / "cache").c_str(), 1);
  grparse::ModelShapePolicy shapes;
  shapes.profiles.push_back(grparse::static_profile({{1, 3, 48, 192}}));
  const grparse::trt::EngineCounts before = grparse::trt::engine_counts();
  auto first = engine_for(scratch / "a" / source.filename(), "cache check", shapes);
  const grparse::trt::EngineCounts built = grparse::trt::engine_counts();
  require_equal(built.built, before.built + 1, "the first load builds the engine");
  size_t engines = 0;
  for (const auto& entry : std::filesystem::directory_iterator(scratch / "cache")) {
    engines += entry.path().extension() == ".engine" ? 1 : 0;
  }
  require_equal(engines, size_t{1}, "the built engine is in the cache directory");
  auto second = engine_for(scratch / "b" / source.filename(), "cache check", shapes);
  const grparse::trt::EngineCounts loaded = grparse::trt::engine_counts();
  require_equal(loaded.built, built.built, "the second load builds nothing");
  require_equal(loaded.loaded_from_cache, built.loaded_from_cache + 1, "it loads from the cache");
  const std::vector<float> crop(3 * 48 * 192, 0.25F);
  const TensorView view{TensorElement::kFloat32, crop.data(), {1, 3, 48, 192}};
  const Tensor one = first->run(std::span(&view, 1)).front();
  const Tensor two = second->run(std::span(&view, 1)).front();
  require(max_abs_difference(one, two, "cached engine") == 0.0, "the cached engine computes the same");
  ::unsetenv("GRPARSE_TENSORRT_CACHE_DIR");
  std::filesystem::remove_all(scratch);
}

// A file TensorRT cannot import is a counted refusal with the parser's own
// words, leaves no placement, and never becomes an ONNX Runtime session.
void verify_unbuildable_model_is_refused() {
  const auto path = std::filesystem::temp_directory_path() / std::format("grparse-trt-bad-{}.onnx", ::getpid());
  { std::ofstream out(path, std::ios::binary); out << "this is not an ONNX model"; }
  grparse::reset_ort_placements();
  const uint64_t before = grparse::ep_fallback_count();
  std::string message;
  try {
    (void)engine_for(path, "broken model");
  } catch (const std::exception& error) {
    message = error.what();
  }
  std::filesystem::remove(path);
  require(message.contains("TensorRT (CUDA device") && message.contains("broken model") &&
              message.contains("will not run on ONNX Runtime or on CPU"),
          "the refusal names TensorRT and the policy: " + message);
  require_equal(grparse::ep_fallback_count(), before + 1, "the refusal is counted");
  require(grparse::ort_placements().empty(), "a refused model records no placement");
}

}  // namespace

int main() {
  const bool gpu_required = std::getenv("GRPARSE_TEST_REQUIRE_GPU") != nullptr &&
                            std::string_view(std::getenv("GRPARSE_TEST_REQUIRE_GPU")) == "1";
  std::string detail;
  if (!grparse::tensorrt_device_present(0, &detail)) {
    std::println(stderr, "tensorrt-model-test: no CUDA device ({}); {}", detail,
                 gpu_required ? "required" : "skipped");
    return gpu_required ? EXIT_FAILURE : 77;
  }
  if (!std::filesystem::exists(models_dir() / "ch_PP-OCRv3_det_infer.onnx")) {
    std::println(stderr, "tensorrt-model-test: no models in {}", models_dir().string());
    return grparse_test::missing_model_exit_code();
  }
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kCuda;
  grparse::set_ort_ep_selection(selection);
  static const cv::Mat page = test_page();
  return grparse_test::run_test_main("tensorrt-model-test", "all checks passed", {
      [] { verify_ocr_nets(page); },
      [] { verify_layout(page); },
      [] { verify_fixed_shape_models(page); },
      verify_engine_cache,
      verify_unbuildable_model_is_refused,
  });
}
