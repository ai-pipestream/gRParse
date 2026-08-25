#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <print>
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
  for (const auto model : {grparse::LayoutModel::kHeron, grparse::LayoutModel::kPicoDet}) {
    std::string message;
    try {
      grparse::LayoutEngine engine("/nonexistent/layout.onnx", model);
    } catch (const std::exception& error) {
      message = error.what();
    }
    require(!message.empty(), "a missing layout model must throw at construction");
    require(message.find(grparse::layout_model_name(model)) != std::string::npos,
            "the failure must name the selected model so the operator knows which file to fetch");
    require(message.find("/nonexistent/layout.onnx") != std::string::npos,
            "the failure must name the path it looked at");
  }
}

struct ExpectedRegion {
  const char* label;
  float score;
  float left, top, right, bottom;
};

// Reference output of the handler this decode is golden-tested against on
// tests/data/report_page.png (conf 0.5, IoU 0.5).  The C++ decode must
// reproduce it: same labels, same geometry within IoU 0.9, scores within
// 0.02.  "figure" in the reference vocabulary is this codebase's "picture".
const ExpectedRegion kReportPagePicoDet[] = {
    {"title", 0.897626F, 122.04F, 72.33F, 850.09F, 148.25F},
    {"text", 0.862554F, 119.81F, 198.42F, 1239.65F, 475.38F},
    {"text", 0.834251F, 118.61F, 510.68F, 1239.83F, 784.28F},
    {"picture", 0.573748F, 119.73F, 857.73F, 1121.28F, 1563.35F},
};

// Output of the query-based detector on the same page, captured from this
// implementation against the provisioned model file (see models/README.md for
// its sha256).  It pins the whole path - preprocessing, the width-first
// original size, the gates, and the label remap - so a change in any of them
// is visible rather than silent.
const ExpectedRegion kReportPageHeron[] = {
    {"table", 0.9829F, 119.0F, 860.0F, 1122.0F, 1140.0F},
    {"text", 0.9163F, 120.0F, 205.0F, 1240.0F, 471.0F},
    {"text", 0.9122F, 120.0F, 515.0F, 1240.0F, 780.0F},
    {"picture", 0.9057F, 118.0F, 1200.0F, 701.0F, 1562.0F},
    {"section_header", 0.6446F, 121.0F, 83.0F, 849.0F, 140.0F},
    {"caption", 0.6401F, 731.0F, 1354.0F, 1023.0F, 1377.0F},
    {"section_header", 0.5678F, 121.0F, 83.0F, 849.0F, 140.0F},
    {"text", 0.5563F, 731.0F, 1354.0F, 1023.0F, 1377.0F},
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

void verify_matches_reference(grparse::LayoutEngine& engine,
                              const std::vector<grparse::LayoutRegion>& regions,
                              const ExpectedRegion* expected, size_t expected_count) {
  require(regions.size() == expected_count,
          "detection count diverged from the reference (got " + std::to_string(regions.size()) +
              ", expected " + std::to_string(expected_count) + ")");
  // Layout must be structure, not a flat text dump.
  bool has_non_text = false;
  for (const auto& region : regions) has_non_text |= region.label != "text";
  require(has_non_text, "report page must not be labelled 100% plain text");

  const auto& vocabulary = engine.labels();
  for (const auto& region : regions) {
    require(std::ranges::find(vocabulary, region.label) != vocabulary.end(),
            "every detection speaks the model's own vocabulary (got " + region.label + ")");
  }
  for (size_t index = 0; index < expected_count; ++index) {
    bool matched = false;
    for (const auto& region : regions) {
      if (region.label == expected[index].label && iou(region, expected[index]) > 0.9F &&
          std::abs(region.confidence - expected[index].score) < 0.02F) {
        matched = true;
        break;
      }
    }
    require(matched, std::string("no detection matched reference ") + expected[index].label);
  }
}

// The layout session is shared by every inference worker, so the same engine
// must answer concurrent pages and stay deterministic doing it.
void verify_shared_session_serves_repeat_calls(grparse::LayoutEngine& engine,
                                               const cv::Mat& image,
                                               const std::vector<grparse::LayoutRegion>& first) {
  const auto again = engine.detect_regions(image);
  require(again.size() == first.size(), "repeat detection must be deterministic");
  for (size_t index = 0; index < again.size(); ++index) {
    require(again[index].label == first[index].label && again[index].left == first[index].left &&
                again[index].top == first[index].top,
            "the shared session must return the same regions in the same order");
  }
  require(engine.stats().detections >= 2, "the shared session counts every page it serves");
}

void verify_rejects_empty_image(const fs::path& model, grparse::LayoutModel selection) {
  grparse::LayoutEngine engine(model, selection);
  bool threw = false;
  try {
    engine.detect_regions(cv::Mat());
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "an empty image must be rejected");
}

// One model's whole leg: skipped when its file is not provisioned, so a host
// that fetched only one of the two still proves that one.
bool run_model(grparse::LayoutModel selection, const fs::path& models_dir, const cv::Mat& image,
               const ExpectedRegion* expected, size_t expected_count) {
  const fs::path model = models_dir / grparse::layout_model_file(selection);
  if (!fs::exists(model)) {
    std::println(stderr, "layout-engine-test: {} leg skipped, no {}",
                 grparse::layout_model_name(selection), model.string());
    return false;
  }
  verify_rejects_empty_image(model, selection);
  grparse::LayoutEngine engine(model, selection);
  require(engine.model() == selection, "the engine reports the model it was built for");
  const auto regions = engine.detect_regions(image);
  verify_matches_reference(engine, regions, expected, expected_count);
  verify_shared_session_serves_repeat_calls(engine, image, regions);
  return true;
}

}  // namespace

int main() {
  try {
    verify_missing_model_fails_loudly();

    const char* models_dir_env = std::getenv("GRPARSE_TEST_MODELS_DIR");
    const char* data_dir = std::getenv("GRPARSE_TEST_DATA_DIR");
    const fs::path models_dir = models_dir_env == nullptr ? "models" : models_dir_env;
    const fs::path image_path =
        fs::path(data_dir == nullptr ? "tests/data" : data_dir) / "report_page.png";
    require(fs::exists(image_path), "test image missing: " + image_path.string());
    const cv::Mat image = cv::imread(image_path.string(), cv::IMREAD_COLOR);
    require(!image.empty(), "test image must load: " + image_path.string());

    const bool heron = run_model(grparse::LayoutModel::kHeron, models_dir, image, kReportPageHeron,
                                 std::size(kReportPageHeron));
    const bool picodet = run_model(grparse::LayoutModel::kPicoDet, models_dir, image,
                                   kReportPagePicoDet, std::size(kReportPagePicoDet));
    if (!heron && !picodet) {
      std::println(stderr, "layout-engine-test: skipped, no layout model present in {:?}",
                   models_dir.string());
      return kSkipExitCode;
    }
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "layout-engine-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
