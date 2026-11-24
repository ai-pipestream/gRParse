// The Hailo OCR page pipeline over fake fixed-shape models: a text line that
// crosses a detector tile seam must come back as one box, and a line wider
// than the recognizer window must read the same as its ink says, once.  The
// fakes answer from the pixels they are handed, so what the test checks is
// the tiling, the stitching and the bookkeeping around three run() calls.

#include <algorithm>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "../src/hailo/hailo_ocr.h"
#include "../src/hailo/hailo_ocr_plan.h"
#include "grparse/ocr_engine.h"
#include "support/check.h"

namespace {

using grparse::InferenceModel;
using grparse::Tensor;
using grparse::TensorElement;
using grparse::TensorView;
using grparse_test::require;
using grparse_test::require_equal;

constexpr int kTile = 640;
constexpr int kRecWidth = 320;
constexpr int kClsWidth = 192;
// Ink levels of the test page: 'a' is darker than 'b'; paper is white.
constexpr uint8_t kInkA = 40;
constexpr uint8_t kInkB = 100;

class FakeModel : public InferenceModel {
 public:
  void prewarm(std::span<const std::vector<grparse::TensorSpec>>) override {}
  const std::vector<std::string>& input_names() const override { return names_; }
  const std::vector<std::string>& output_names() const override { return names_; }
  std::string_view backend_name() const override { return "fake"; }
  int64_t frames = 0;
  std::vector<int64_t> calls;

 protected:
  static Tensor floats(std::vector<int64_t> shape, const std::vector<float>& values) {
    Tensor tensor;
    tensor.element = TensorElement::kFloat32;
    tensor.shape = std::move(shape);
    const auto* bytes = reinterpret_cast<const std::byte*>(values.data());
    tensor.bytes.assign(bytes, bytes + values.size() * sizeof(float));
    return tensor;
  }
  void count(const TensorView& input, int height, int width) {
    require(input.element == TensorElement::kUInt8 && input.shape.size() == 4 &&
                input.shape[1] == height && input.shape[2] == width && input.shape[3] == 3,
            "a device model is fed uint8 [frames, height, width, 3] at its one size");
    frames += input.shape[0];
    calls.push_back(input.shape[0]);
  }

 private:
  std::vector<std::string> names_{"tensor"};
};

// Probability 1 on ink, 0 on paper.
class FakeDetection final : public FakeModel {
 public:
  bool fail = false;
  std::vector<Tensor> run(std::span<const TensorView> inputs) override {
    if (fail) throw grparse::InferenceDeviceError("Hailo device failed OCR detection: unplugged");
    count(inputs[0], kTile, kTile);
    const auto* pixels = static_cast<const uint8_t*>(inputs[0].data);
    const size_t area = static_cast<size_t>(kTile) * kTile;
    std::vector<float> map(static_cast<size_t>(inputs[0].shape[0]) * area);
    for (size_t i = 0; i < map.size(); ++i) map[i] = pixels[i * 3] < 128 ? 1.0F : 0.0F;
    return {floats({inputs[0].shape[0], kTile, kTile, 1}, map)};
  }
};

class FakeDirection final : public FakeModel {
 public:
  float flipped = 0.01F;
  std::vector<Tensor> run(std::span<const TensorView> inputs) override {
    count(inputs[0], grparse::hailo::kRecHeight, kClsWidth);
    std::vector<float> scores;
    for (int64_t i = 0; i < inputs[0].shape[0]; ++i) {
      scores.push_back(1.0F - flipped);
      scores.push_back(flipped);
    }
    return {floats({inputs[0].shape[0], 1, 1, 2}, scores)};
  }
};

// One-hot per time step from the ink at the step's first pixel on the line's
// middle row: class 1 for 'a' ink, 2 for 'b' ink, blank for paper.  The
// padding grey reads as 'b', so a step decoded past the line's end shows up.
class FakeRecognition final : public FakeModel {
 public:
  std::vector<Tensor> run(std::span<const TensorView> inputs) override {
    count(inputs[0], grparse::hailo::kRecHeight, kRecWidth);
    const auto* pixels = static_cast<const uint8_t*>(inputs[0].data);
    const int steps = kRecWidth / grparse::hailo::kRecStepPixels;
    const size_t frame = static_cast<size_t>(grparse::hailo::kRecHeight) * kRecWidth * 3;
    std::vector<float> scores(static_cast<size_t>(inputs[0].shape[0]) * steps * 4, 0.0F);
    for (int64_t i = 0; i < inputs[0].shape[0]; ++i) {
      const uint8_t* row = pixels + static_cast<size_t>(i) * frame +
                           static_cast<size_t>(grparse::hailo::kRecHeight / 2) * kRecWidth * 3;
      for (int step = 0; step < steps; ++step) {
        const uint8_t ink = row[step * grparse::hailo::kRecStepPixels * 3];
        const int index = ink < 70 ? 1 : ink < 160 ? 2 : 0;
        scores[(static_cast<size_t>(i) * steps + step) * 4 + index] = 0.9F;
      }
    }
    return {floats({inputs[0].shape[0], 1, steps, 4}, scores)};
  }
};

struct Rig {
  FakeDetection detection;
  FakeDirection direction;
  FakeRecognition recognition;
  grparse::hailo::HailoOcr ocr{
      grparse::hailo::OcrModels{&detection, &direction, &recognition,
                                grparse::hailo::OcrShapes{kTile, kClsWidth, kRecWidth, 4}},
      {"#", "a", "b", " "}};
};

// A 1000x400 page with one 900x30 line starting and ending in 'b' ink:
// 22 segments of about 41 pixels, b a b a ... b.
cv::Mat page_with_one_line(std::string* text) {
  cv::Mat page(400, 1000, CV_8UC3, cv::Scalar(255, 255, 255));
  const int segments = 21;
  for (int i = 0; i < segments; ++i) {
    const bool a = i % 2 == 1;
    const int left = 50 + i * 900 / segments;
    const int right = 50 + (i + 1) * 900 / segments;
    const uint8_t ink = a ? kInkA : kInkB;
    cv::rectangle(page, cv::Rect(left, 100, right - left, 30), cv::Scalar(ink, ink, ink),
                  cv::FILLED);
    *text += a ? 'a' : 'b';
  }
  return page;
}

grparse::OcrDetectOptions options() { return grparse::OcrDetectOptions{}; }

void verify_a_line_across_seams_is_one_box_read_once() {
  Rig rig;
  std::string text;
  const cv::Mat page = page_with_one_line(&text);
  const std::vector<TextBlock> blocks = rig.ocr.detect(page, options());
  require_equal(blocks.size(), size_t{1}, "the line crosses a tile seam and is still one box");
  int left = page.cols;
  int right = 0;
  int top = page.rows;
  int bottom = 0;
  for (const cv::Point& point : blocks[0].boxPoint) {
    left = std::min(left, point.x);
    right = std::max(right, point.x);
    top = std::min(top, point.y);
    bottom = std::max(bottom, point.y);
  }
  require(left <= 50 && left >= 0 && right >= 950 && right <= page.cols,
          std::format("the box spans the line in page coordinates, got {}..{}", left, right));
  require(top <= 100 && bottom >= 130 && bottom <= 200,
          std::format("the box covers the line's rows, got {}..{}", top, bottom));
  require_equal(blocks[0].text, text, "windows joined at a seam read each glyph once");
  require_equal(blocks[0].charScores.size(), text.size(), "one score per character");
  require_equal(blocks[0].angleIndex, 0, "an upright page is not flipped");
  require_equal(rig.detection.frames, int64_t{2}, "a 1088 wide page is two detector tiles");
  require_equal(rig.detection.calls.size(), size_t{1}, "sent as one batch");
  require_equal(rig.direction.frames, int64_t{1}, "one crop through the direction net");
  require(rig.recognition.frames >= 2, "a line wider than the window is several windows");
  require_equal(rig.recognition.calls.size(), size_t{1}, "sent as one batch");
}

void verify_a_flipped_page_is_turned_before_reading() {
  Rig rig;
  rig.direction.flipped = 0.99F;
  std::string text;
  const cv::Mat page = page_with_one_line(&text);
  const std::vector<TextBlock> blocks = rig.ocr.detect(page, options());
  require_equal(blocks.size(), size_t{1}, "one line");
  require_equal(blocks[0].angleIndex, 1, "a sure flip turns the crop");
  std::string reversed(text.rbegin(), text.rend());
  require_equal(blocks[0].text, reversed, "the turned crop reads right to left");
}

void verify_a_blank_page_runs_only_the_detector() {
  Rig rig;
  const cv::Mat page(300, 300, CV_8UC3, cv::Scalar(255, 255, 255));
  require(rig.ocr.detect(page, options()).empty(), "a blank page has no lines");
  require_equal(rig.detection.frames, int64_t{1}, "a small page is one padded tile");
  require_equal(rig.direction.frames + rig.recognition.frames, int64_t{0},
                "no crop, no direction or recognition frame");
}

void verify_a_device_failure_reaches_the_caller() {
  Rig rig;
  rig.detection.fail = true;
  std::string text;
  const cv::Mat page = page_with_one_line(&text);
  bool threw = false;
  try {
    (void)rig.ocr.detect(page, options());
  } catch (const grparse::InferenceDeviceError& error) {
    threw = std::string(error.what()).contains("Hailo device failed");
  }
  require(threw, "a device error is not swallowed or answered from anywhere else");
}

void verify_the_dictionary_must_match_the_recognizer() {
  FakeDetection detection;
  FakeDirection direction;
  FakeRecognition recognition;
  bool threw = false;
  try {
    grparse::hailo::HailoOcr ocr(
        grparse::hailo::OcrModels{&detection, &direction, &recognition,
                                  grparse::hailo::OcrShapes{kTile, kClsWidth, kRecWidth, 6625}},
        {"#", "a", "b", " "});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "a recognizer with another class count than the dictionary is refused");
  threw = false;
  try {
    grparse::hailo::HailoOcr ocr(
        grparse::hailo::OcrModels{&detection, nullptr, &recognition,
                                  grparse::hailo::OcrShapes{kTile, kClsWidth, kRecWidth, 4}},
        {"#", "a", "b", " "});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "a missing model is refused");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("hailo-ocr-test", "tiles and windows join into the page's lines", {
      verify_a_line_across_seams_is_one_box_read_once,
      verify_a_flipped_page_is_turned_before_reading,
      verify_a_blank_page_runs_only_the_detector,
      verify_a_device_failure_reaches_the_caller,
      verify_the_dictionary_must_match_the_recognizer,
  });
}
