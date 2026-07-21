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

OcrEngine::Page OcrEngine::extract_page(const std::filesystem::path& image_path) {
  const auto image = cv::imread(image_path.string(), cv::IMREAD_COLOR);
  if (image.empty()) {
    throw std::runtime_error("RapidOCR could not decode image: " + image_path.string());
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto result = engine_->detect(image, 50, 2048, 0.6f, 0.3f, 2.0f, true, true);
  Page page{image.cols, image.rows, {}};
  page.lines.reserve(result.textBlocks.size());
  for (const auto& block : result.textBlocks) {
    page.lines.push_back(Line{block.text, block.boxPoint});
  }
  return page;
}

}  // namespace grparse
