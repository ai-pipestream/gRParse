#include "grparse/ocr_engine.h"

#include "grparse/text_geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>

#include <opencv2/core.hpp>

#include "grparse_session_ep.h"

namespace grparse {
namespace {

[[noreturn]] void reject(const char* name, const char* expectation) {
  throw std::invalid_argument(std::string(name) + " must be " + expectation);
}

int env_int(const char* name, int fallback, int minimum, int maximum) {
  const char* configured = std::getenv(name);
  if (configured == nullptr || *configured == '\0') return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(configured, &end, 10);
  if (*end != '\0' || parsed < minimum || parsed > maximum) {
    reject(name, ("an integer between " + std::to_string(minimum) + " and " +
                  std::to_string(maximum))
                     .c_str());
  }
  return static_cast<int>(parsed);
}

float env_float(const char* name, float fallback, float minimum, float maximum) {
  const char* configured = std::getenv(name);
  if (configured == nullptr || *configured == '\0') return fallback;
  char* end = nullptr;
  const float parsed = std::strtof(configured, &end);
  if (*end != '\0' || !(parsed >= minimum) || !(parsed <= maximum)) {
    reject(name, ("a number between " + std::to_string(minimum) + " and " +
                  std::to_string(maximum))
                     .c_str());
  }
  return parsed;
}

OcrDetectOptions detect_options_from_env() {
  const OcrDetectOptions defaults;
  OcrDetectOptions options;
  options.padding = env_int("GRPARSE_OCR_PADDING", defaults.padding, 0, 4096);
  options.max_side_len = env_int("GRPARSE_OCR_MAX_SIDE", defaults.max_side_len, 32, 16384);
  options.box_score_thresh = env_float("GRPARSE_OCR_BOX_SCORE", defaults.box_score_thresh, 0.0F, 1.0F);
  options.box_thresh = env_float("GRPARSE_OCR_BOX_THRESH", defaults.box_thresh, 0.0F, 1.0F);
  options.un_clip_ratio = env_float("GRPARSE_OCR_UNCLIP", defaults.un_clip_ratio, 0.1F, 10.0F);
  return options;
}

int round_down_32(int value) { return std::max(32, value / 32 * 32); }

int round_up_32(int value) { return std::max(32, (value + 31) / 32 * 32); }

}  // namespace

int ocr_detector_side(int longest_side, int max_side_len) {
  const int cap = round_down_32(max_side_len);
  // The ladder below the cap, then the cap.
  std::array<int, std::size(kOcrDetectorLadder) + 1> sides{};
  size_t count = 0;
  for (const int side : kOcrDetectorLadder) {
    if (side < cap) sides[count++] = side;
  }
  sides[count++] = cap;
  if (longest_side <= sides[0]) return sides[0];
  for (size_t index = 1; index < count; ++index) {
    if (longest_side > sides[index]) continue;
    // Between two sizes: the nearer by ratio, the larger on a tie, so the
    // page is scaled by as little as the ladder allows either way.
    const int64_t below = sides[index - 1];
    const int64_t above = sides[index];
    const int64_t side = longest_side;
    return side * side < below * above ? sides[index - 1] : sides[index];
  }
  return sides[count - 1];
}

OcrDetectGeometry ocr_detect_geometry(int page_width, int page_height, int padding,
                                      int max_side_len) {
  const int padded_width = page_width + 2 * padding;
  const int padded_height = page_height + 2 * padding;
  const int longest = std::max(padded_width, padded_height);
  const int side = ocr_detector_side(longest, max_side_len);
  OcrDetectGeometry geometry;
  if (longest <= ocr_detector_side(1, max_side_len)) {
    // Below the ladder's first size: padded out to it, not scaled up, so
    // the canvas is the input and white past the page.
    const bool wide = padded_width >= padded_height;
    geometry.input_width = wide ? side : std::min(side, round_up_32(padded_width));
    geometry.input_height = wide ? std::min(side, round_up_32(padded_height)) : side;
    geometry.canvas_width = geometry.input_width;
    geometry.canvas_height = geometry.input_height;
    return geometry;
  }
  // Scaled by side / longest on both axes, so pages of one shape share one
  // input whatever their size.  The longest side maps exactly; the other
  // rounds up to a multiple of 32, and the canvas grows on that axis by
  // what the extra input pixels cover, so the resize stays isotropic and
  // the page keeps its aspect ratio.
  const double scale = static_cast<double>(side) / static_cast<double>(longest);
  const auto letterbox = [&](int padded, int* input, int* canvas) {
    if (padded == longest) {
      *input = side;
      *canvas = padded;
      return;
    }
    *input = std::min(side, round_up_32(static_cast<int>(std::ceil(padded * scale))));
    *canvas = std::max(padded, static_cast<int>(std::lround(*input / scale)));
  };
  letterbox(padded_width, &geometry.input_width, &geometry.canvas_width);
  letterbox(padded_height, &geometry.input_height, &geometry.canvas_height);
  return geometry;
}

const OcrDetectOptions& ocr_detect_options() {
  // Parsed once: extract_page() runs per page and must not re-read the
  // environment on the hot path.
  static const OcrDetectOptions options = detect_options_from_env();
  return options;
}

OcrEngine::OcrEngine(const std::filesystem::path& model_directory, int gpu_index) {
  const auto det = model_directory / "ch_PP-OCRv3_det_infer.onnx";
  const auto cls = model_directory / "ch_ppocr_mobile_v2.0_cls_infer.onnx";
  const auto rec = model_directory / "ch_PP-OCRv3_rec_infer.onnx";
  const auto keys = model_directory / "ppocr_keys_v1.txt";
  for (const auto& model : {det, cls, rec, keys}) {
    if (!std::filesystem::is_regular_file(model)) {
      throw std::runtime_error("Required OCR model is missing: " + model.string());
    }
  }
  // Surface a malformed GRPARSE_OCR_* value at startup, not on the first page.
  ocr_detect_options();

  engine_ = std::make_unique<OcrLite>();
  engine_->setGpuIndex(gpu_index);
  engine_->setNumThread(1);
  if (!engine_->initModels(det.string(), cls.string(), rec.string(), keys.string())) {
    // An execution-provider failure does not reach this line: the patched
    // nets retry and then throw from initModel, failing startup loudly.  A
    // false here means the model wiring itself failed.
    throw std::runtime_error("RapidOCR failed to initialize its models");
  }
}

OcrEngine::Page OcrEngine::extract_page(const cv::Mat& image) {
  // The first inference on a fresh session compiles the provider's kernels;
  // serialize it process-wide (see grparse_session_ep.h).
  std::optional<OvCompileGate> compile_gate;
  if (!warmed_) compile_gate.emplace();
  if (image.empty()) {
    throw std::runtime_error("RapidOCR could not decode the in-memory image");
  }
  const OcrDetectOptions& options = ocr_detect_options();
  // The page on a white canvas sized for one of a few detector inputs (see
  // ocr_detect_geometry), so pages of every size share a handful of shapes.
  // One border call does the padding RapidOCR would have added and the
  // letterbox margin together.
  const OcrDetectGeometry geometry =
      ocr_detect_geometry(image.cols, image.rows, options.padding, options.max_side_len);
  cv::Mat canvas;
  cv::copyMakeBorder(image, canvas, options.padding,
                     geometry.canvas_height - image.rows - options.padding, options.padding,
                     geometry.canvas_width - image.cols - options.padding, cv::BORDER_ISOLATED,
                     cv::Scalar(255, 255, 255));
  cv::Rect page_rect(options.padding, options.padding, image.cols, image.rows);
  ScaleParam scale{
      geometry.canvas_width,
      geometry.canvas_height,
      geometry.input_width,
      geometry.input_height,
      static_cast<float>(geometry.input_width) / static_cast<float>(geometry.canvas_width),
      static_cast<float>(geometry.input_height) / static_cast<float>(geometry.canvas_height)};
  const auto result =
      engine_->detect(nullptr, nullptr, canvas, page_rect, scale, options.box_score_thresh,
                      options.box_thresh, options.un_clip_ratio, options.do_angle, options.most_angle);
  Page page{image.cols, image.rows, {}};
  page.source = OcrPage::Source::kOcr;
  page.lines.reserve(result.textBlocks.size());
  for (const auto& block : result.textBlocks) {
    float confidence = block.boxScore;
    if (!block.charScores.empty()) {
      confidence = 0.0F;
      for (const float score : block.charScores) confidence += score;
      confidence /= static_cast<float>(block.charScores.size());
    }
    Line line{block.text, block.boxPoint, confidence, TextOrigin::kOcr};
    line.flipped = block.angleIndex == 1;
    // The detector's unclip ratio grows a box past the glyphs, so a line
    // at the edge of the raster can reach outside it; the box is where
    // the line is seen, and nothing past the edge is.
    clip_to_raster(&line, image.cols, image.rows);
    page.lines.push_back(std::move(line));
  }
  warmed_ = true;
  return page;
}

OcrEnginePool::OcrEnginePool(const std::filesystem::path& model_directory, size_t worker_count,
                             int gpu_index)
    : engines_(worker_count, [model_directory, gpu_index] {
        return std::make_unique<OcrEngine>(model_directory, gpu_index);
      }) {
  engines_.prime();
}

OcrPage OcrEnginePool::extract_page(const cv::Mat& image) {
  auto lease = engines_.acquire();
  try {
    return lease->extract_page(image);
  } catch (...) {
    // A throwing inference may leave the ORT session wedged (device reset,
    // OOM mid-run).  Destroy it so the next acquire builds a fresh session
    // instead of feeding every later page to a poisoned one.
    lease.discard();
    throw;
  }
}

}  // namespace grparse
