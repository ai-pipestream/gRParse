#include "grparse/ocr_engine.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

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

}  // namespace

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
    throw std::runtime_error("RapidOCR failed to initialize its CUDA models");
  }
}

OcrEngine::Page OcrEngine::extract_page(const cv::Mat& image) {
  if (image.empty()) {
    throw std::runtime_error("RapidOCR could not decode the in-memory image");
  }
  const OcrDetectOptions& options = ocr_detect_options();
  const auto result =
      engine_->detect(image, options.padding, options.max_side_len, options.box_score_thresh,
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
    page.lines.push_back(Line{block.text, block.boxPoint, confidence, TextOrigin::kOcr});
  }
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
  return lease->extract_page(image);
}

}  // namespace grparse
