#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "grparse/figure_classifier.h"
#include "grparse_session_ep.h"
#include "support/check.h"

namespace {

namespace fs = std::filesystem;

using grparse_test::require;

void verify_missing_model_fails_loudly() {
  bool threw = false;
  try {
    grparse::FigureClassifierEngine engine("/nonexistent/classifier.onnx");
  } catch (const std::exception&) {
    threw = true;
  }
  require(threw, "a missing classifier model must throw at construction");
}

// The committed bar chart fixture is unambiguous: the reference classifier
// calls it bar_chart with near-certainty.  OpenCV resizing differs slightly
// from the reference's, so the assertion is the decisive call and the shape of
// the distribution, not an exact probability.
void verify_bar_chart_matches_reference(const fs::path& model, const fs::path& image_path) {
  grparse::FigureClassifierPool pool(model, 2);
  const cv::Mat image = cv::imread(image_path.string(), cv::IMREAD_COLOR);
  require(!image.empty(), "test image must load: " + image_path.string());

  const auto classes = pool.classify(image);
  require(grparse::FigureClassifierEngine::labels().size() == 26,
          "the classifier predicts 26 classes");
  require(classes.size() == grparse::FigureClassifierEngine::labels().size(),
          "every class must be scored");
  require(classes.front().label == "bar_chart",
          "bar chart fixture must classify as bar_chart (got " + classes.front().label + ")");
  require(classes.front().confidence > 0.9F, "the reference call is near-certain");
  float total = 0.0F;
  for (const auto& entry : classes) total += entry.confidence;
  require(total > 0.99F && total < 1.01F, "confidences must be a probability distribution");

  const auto again = pool.classify(image);
  require(again.front().label == classes.front().label, "repeat classification is deterministic");
  require(pool.stats().acquires >= 2 && pool.stats().discards == 0,
          "pool must lease warm sessions without discards");
}

// Under the OpenVINO provider the engine fixes the model's free batch
// dimension at session build (unit_batch_overrides), so the network is
// compiled once instead of on the first figure.  The fixed network must make
// the same call as the CPU provider's.  Runs on the OpenVINO CPU device, so
// it needs no GPU; a build without the provider has nothing to check.
void verify_openvino_fixed_batch_agrees(const fs::path& model, const fs::path& image_path) {
  const std::vector<std::string> providers = Ort::GetAvailableProviders();
  if (std::ranges::find(providers, "OpenVINOExecutionProvider") == providers.end()) return;
  const cv::Mat image = cv::imread(image_path.string(), cv::IMREAD_COLOR);
  grparse::FigureClassifierEngine on_cpu(model);
  const auto expected = on_cpu.classify(image);

  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "grparse-test");
  require(grparse::unit_batch_overrides(env, model).size() == 1,
          "the classifier model has one free batch dimension to fix");
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kOpenVino;
  selection.openvino_device = "CPU";
  grparse::set_ort_ep_selection(selection);
  grparse::FigureClassifierEngine on_openvino(model);
  const auto actual = on_openvino.classify(image);
  selection.ep = grparse::OrtEp::kCpu;
  grparse::set_ort_ep_selection(selection);

  require(actual.front().label == expected.front().label,
          "the fixed-batch OpenVINO session calls the fixture " + actual.front().label +
              ", the CPU session " + expected.front().label);
  require(std::abs(actual.front().confidence - expected.front().confidence) < 0.02F,
          "the fixed-batch OpenVINO confidence is within 0.02 of the CPU session's");
}

void verify_rejects_empty_image(const fs::path& model) {
  grparse::FigureClassifierEngine engine(model);
  bool threw = false;
  try {
    engine.classify(cv::Mat());
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "an empty crop must be rejected");
}

}  // namespace

int main() {
  try {
    verify_missing_model_fails_loudly();

    const char* models_dir = std::getenv("GRPARSE_TEST_MODELS_DIR");
    const char* data_dir = std::getenv("GRPARSE_TEST_DATA_DIR");
    const fs::path model =
        fs::path(models_dir == nullptr ? "models" : models_dir) / "figure_classifier.onnx";
    const fs::path image =
        fs::path(data_dir == nullptr ? "tests/data" : data_dir) / "bar_chart.png";
    if (!fs::exists(model)) {
      std::println(stderr, "figure-classifier-test: skipped, model not present: {:?}", model.string());
      return grparse_test::missing_model_exit_code();
    }
    require(fs::exists(image), "test image missing: " + image.string());

    verify_rejects_empty_image(model);
    verify_bar_chart_matches_reference(model, image);
    verify_openvino_fixed_batch_agrees(model, image);
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "figure-classifier-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
