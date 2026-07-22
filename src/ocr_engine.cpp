#include "grparse/ocr_engine.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

#include <opencv2/imgcodecs.hpp>

namespace grparse {
namespace {

struct OcrDetectOptions {
  int padding = 50;
  int max_side_len = 2048;
  float box_score_thresh = 0.6F;
  float box_thresh = 0.3F;
  float un_clip_ratio = 2.0F;
  bool do_angle = true;
  bool most_angle = true;
};

int env_int(const char* name, int fallback) {
  const char* configured = std::getenv(name);
  if (configured == nullptr) return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(configured, &end, 10);
  if (end == configured || *end != '\0') return fallback;
  return static_cast<int>(parsed);
}

float env_float(const char* name, float fallback) {
  const char* configured = std::getenv(name);
  if (configured == nullptr) return fallback;
  char* end = nullptr;
  const float parsed = std::strtof(configured, &end);
  if (end == configured || *end != '\0') return fallback;
  return parsed;
}

OcrDetectOptions detect_options_from_env() {
  OcrDetectOptions options;
  options.padding = env_int("GRPARSE_OCR_PADDING", options.padding);
  options.max_side_len = env_int("GRPARSE_OCR_MAX_SIDE", options.max_side_len);
  options.box_score_thresh = env_float("GRPARSE_OCR_BOX_SCORE", options.box_score_thresh);
  options.box_thresh = env_float("GRPARSE_OCR_BOX_THRESH", options.box_thresh);
  options.un_clip_ratio = env_float("GRPARSE_OCR_UNCLIP", options.un_clip_ratio);
  return options;
}

}  // namespace

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
  const OcrDetectOptions options = detect_options_from_env();
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
    page.lines.push_back(
        Line{block.text, block.boxPoint, confidence, TextOrigin::kOcr});
  }
  return page;
}

OcrEnginePool::Lease::Lease(OcrEnginePool* pool, size_t index) : pool_(pool), index_(index) {}

OcrEnginePool::Lease::Lease(Lease&& other) noexcept : pool_(other.pool_), index_(other.index_) {
  other.pool_ = nullptr;
}

OcrEnginePool::Lease& OcrEnginePool::Lease::operator=(Lease&& other) noexcept {
  if (this != &other) {
    release();
    pool_ = other.pool_;
    index_ = other.index_;
    other.pool_ = nullptr;
  }
  return *this;
}

OcrEnginePool::Lease::~Lease() { release(); }

OcrEngine& OcrEnginePool::Lease::engine() const { return *pool_->engines_.at(index_); }

void OcrEnginePool::Lease::release() {
  if (pool_ != nullptr) {
    pool_->release(index_);
    pool_ = nullptr;
  }
}

OcrEnginePool::OcrEnginePool(const std::filesystem::path& model_directory, size_t worker_count,
                             int gpu_index) {
  if (worker_count == 0) throw std::invalid_argument("OCR worker count must be positive");
  engines_.reserve(worker_count);
  for (size_t index = 0; index < worker_count; ++index) {
    engines_.push_back(std::make_unique<OcrEngine>(model_directory, gpu_index));
    available_.push_back(index);
  }
}

OcrEnginePool::Lease OcrEnginePool::acquire() {
  std::unique_lock<std::mutex> lock(mutex_);
  available_cv_.wait(lock, [this] { return !available_.empty(); });
  const size_t index = available_.front();
  available_.pop_front();
  return Lease(this, index);
}

OcrEngine::Page OcrEnginePool::extract_page(const cv::Mat& image) {
  auto lease = acquire();
  return lease.engine().extract_page(image);
}

size_t OcrEnginePool::size() const { return engines_.size(); }

void OcrEnginePool::release(size_t index) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    available_.push_back(index);
  }
  available_cv_.notify_one();
}

}  // namespace grparse
