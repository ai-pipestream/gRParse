#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include <opencv2/imgcodecs.hpp>

#include "grparse/figure_classifier.h"

namespace {

namespace fs = std::filesystem;

constexpr int kSkipExitCode = 77;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void verify_missing_model_fails_loudly() {
  bool threw = false;
  try {
    grparse::FigureClassifierEngine engine("/nonexistent/classifier.onnx");
  } catch (const std::exception&) {
    threw = true;
  }
  require(threw, "a missing classifier model must throw at construction");
}

// Reference: the torch DocumentFigureClassifier scores the committed bar
// chart fixture as bar_chart at 0.9997 (see scripts/export_figure_classifier.py).
// OpenCV resizing differs slightly from PIL, so the assertion is the decisive
// call, not the exact probability.
void verify_bar_chart_matches_reference(const fs::path& model, const fs::path& image_path) {
  grparse::FigureClassifierPool pool(model, 2);
  const cv::Mat image = cv::imread(image_path.string(), cv::IMREAD_COLOR);
  require(!image.empty(), "test image must load: " + image_path.string());

  const auto classes = pool.classify(image);
  require(classes.size() == grparse::FigureClassifierEngine::labels().size(),
          "every class must be scored");
  require(classes.front().label == "bar_chart",
          "bar chart fixture must classify as bar_chart (got " + classes.front().label + ")");
  require(classes.front().confidence > 0.9F, "reference confidence is 0.9997");
  float total = 0.0F;
  for (const auto& entry : classes) total += entry.confidence;
  require(total > 0.99F && total < 1.01F, "confidences must be a probability distribution");

  const auto again = pool.classify(image);
  require(again.front().label == classes.front().label, "repeat classification is deterministic");
  require(pool.stats().acquires >= 2 && pool.stats().discards == 0,
          "pool must lease warm sessions without discards");
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
      std::cerr << "figure-classifier-test: skipped, model not present: " << model << '\n';
      return kSkipExitCode;
    }
    require(fs::exists(image), "test image missing: " + image.string());

    verify_rejects_empty_image(model);
    verify_bar_chart_matches_reference(model, image);
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "figure-classifier-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
