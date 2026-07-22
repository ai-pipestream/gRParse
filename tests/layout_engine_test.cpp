#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "grparse/layout_engine.h"

namespace {

namespace fs = std::filesystem;

constexpr int kSkipExitCode = 77;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void verify_missing_model_fails_loudly() {
  bool threw = false;
  try {
    grparse::LayoutEngine engine("/nonexistent/layout.onnx");
  } catch (const std::exception&) {
    threw = true;
  }
  require(threw, "a missing layout model must throw at construction");
}

struct ExpectedRegion {
  const char* label;
  float score;
  float left, top, right, bottom;
};

// Reference output of RapidLayout's pp_layout_publaynet handler on
// tests/data/report_page.png (conf 0.5, IoU 0.5).  The C++ decode must
// reproduce it: same labels, same geometry within IoU 0.9, scores within
// 0.02.  Regenerate with scripts described in models/README.md if the model
// file is ever updated.
const ExpectedRegion kReportPageGolden[] = {
    {"title", 0.897626F, 122.04F, 72.33F, 850.09F, 148.25F},
    {"text", 0.862554F, 119.81F, 198.42F, 1239.65F, 475.38F},
    {"text", 0.834251F, 118.61F, 510.68F, 1239.83F, 784.28F},
    {"figure", 0.573748F, 119.73F, 857.73F, 1121.28F, 1563.35F},
};

float iou(const grparse::LayoutRegion& box, const ExpectedRegion& expected) {
  const float left = std::max(static_cast<float>(box.left), expected.left);
  const float top = std::max(static_cast<float>(box.top), expected.top);
  const float right = std::min(static_cast<float>(box.right), expected.right);
  const float bottom = std::min(static_cast<float>(box.bottom), expected.bottom);
  const float intersection = std::max(0.0F, right - left) * std::max(0.0F, bottom - top);
  const float area_detected = static_cast<float>(box.right - box.left) *
                              static_cast<float>(box.bottom - box.top);
  const float area_expected =
      (expected.right - expected.left) * (expected.bottom - expected.top);
  return intersection / (area_detected + area_expected - intersection);
}

void verify_report_page_matches_reference(const fs::path& model, const fs::path& image_path) {
  grparse::LayoutEnginePool pool(model, 2);
  const cv::Mat image = cv::imread(image_path.string(), cv::IMREAD_COLOR);
  require(!image.empty(), "test image must load: " + image_path.string());

  const auto regions = pool.detect_regions(image);
  require(regions.size() == std::size(kReportPageGolden),
          "detection count diverged from the reference (got " + std::to_string(regions.size()) +
              ")");
  // Layout must be structure, not a flat text dump (epic C acceptance).
  bool has_non_text = false;
  for (const auto& region : regions) has_non_text |= region.label != "text";
  require(has_non_text, "report page must not be labelled 100% plain text");

  for (const auto& expected : kReportPageGolden) {
    bool matched = false;
    for (const auto& region : regions) {
      if (region.label == expected.label && iou(region, expected) > 0.9F &&
          std::abs(region.confidence - expected.score) < 0.02F) {
        matched = true;
        break;
      }
    }
    require(matched, std::string("no detection matched reference ") + expected.label);
  }

  // Warm reuse: a second call must not rebuild sessions.
  const auto again = pool.detect_regions(image);
  require(again.size() == regions.size(), "repeat detection must be deterministic");
  require(pool.stats().acquires >= 2 && pool.stats().discards == 0,
          "pool must lease warm sessions without discards");
}

void verify_rejects_empty_image(const fs::path& model) {
  grparse::LayoutEngine engine(model);
  bool threw = false;
  try {
    engine.detect_regions(cv::Mat());
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "an empty image must be rejected");
}

}  // namespace

int main() {
  try {
    verify_missing_model_fails_loudly();

    const char* models_dir = std::getenv("GRPARSE_TEST_MODELS_DIR");
    const char* data_dir = std::getenv("GRPARSE_TEST_DATA_DIR");
    const fs::path model =
        fs::path(models_dir == nullptr ? "models" : models_dir) / "layout_publaynet.onnx";
    const fs::path image =
        fs::path(data_dir == nullptr ? "tests/data" : data_dir) / "report_page.png";
    if (!fs::exists(model)) {
      std::cerr << "layout-engine-test: skipped, model not present: " << model << '\n';
      return kSkipExitCode;
    }
    require(fs::exists(image), "test image missing: " + image.string());

    verify_rejects_empty_image(model);
    verify_report_page_matches_reference(model, image);
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "layout-engine-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
