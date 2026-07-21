#include "grparse/ocr_engine.h"

#include <stdexcept>

#include <opencv2/imgcodecs.hpp>

namespace grparse {

OcrEngine::OcrEngine(const std::filesystem::path& model_directory) {
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
  engine_->setGpuIndex(0);
  engine_->setNumThread(1);
  if (!engine_->initModels(det.string(), cls.string(), rec.string(), keys.string())) {
    throw std::runtime_error("RapidOCR failed to initialize its CUDA models");
  }
}

OcrEngine::Page OcrEngine::extract_page(const cv::Mat& image) {
  if (image.empty()) {
    throw std::runtime_error("RapidOCR could not decode the in-memory image");
  }
  const auto result = engine_->detect(image, 50, 2048, 0.6f, 0.3f, 2.0f, true, true);
  Page page{image.cols, image.rows, {}};
  page.lines.reserve(result.textBlocks.size());
  for (const auto& block : result.textBlocks) {
    page.lines.push_back(Line{block.text, block.boxPoint});
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

OcrEnginePool::OcrEnginePool(const std::filesystem::path& model_directory, size_t worker_count) {
  if (worker_count == 0) throw std::invalid_argument("OCR worker count must be positive");
  engines_.reserve(worker_count);
  for (size_t index = 0; index < worker_count; ++index) {
    engines_.push_back(std::make_unique<OcrEngine>(model_directory));
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
