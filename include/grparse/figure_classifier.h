#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"
#include "grparse/resource_pool.h"

namespace grparse {

// The scheduler talks to this interface so tests can classify figures
// without a model.  Returns every class with its probability, sorted by
// descending confidence.
class FigureClassifierBase {
 public:
  virtual ~FigureClassifierBase() = default;
  virtual std::vector<FigureClass> classify(const cv::Mat& crop) = 0;
};

// ds4sd/DocumentFigureClassifier (EfficientNet-B0, 16 classes) on ONNX
// Runtime, exported by scripts/export_figure_classifier.py.  Sessions bind
// the process-wide execution provider selection, pooled like OCR sessions.
//
// Anti-seesaw contract: classify is a batch=1 device call on a figure crop
// of the raster the inference stage already holds; it neither retains the
// crop nor blocks on anything but its own inference.
class FigureClassifierEngine final {
 public:
  // Class labels, index == model output index (id2label order).
  static const std::vector<std::string>& labels();

  // Throws when the model is missing or the configured execution provider
  // cannot initialize (startup fail-loud, same policy as OCR).
  explicit FigureClassifierEngine(const std::filesystem::path& model_path);
  ~FigureClassifierEngine();
  FigureClassifierEngine(const FigureClassifierEngine&) = delete;
  FigureClassifierEngine& operator=(const FigureClassifierEngine&) = delete;

  // BGR figure crop in, softmaxed classes sorted by confidence out.
  std::vector<FigureClass> classify(const cv::Mat& crop);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Warm exclusive-lease pool over FigureClassifierEngine.
class FigureClassifierPool final : public FigureClassifierBase {
 public:
  using Stats = ResourcePool<FigureClassifierEngine>::Stats;

  FigureClassifierPool(const std::filesystem::path& model_path, size_t worker_count);

  std::vector<FigureClass> classify(const cv::Mat& crop) override;
  size_t size() const { return engines_.capacity(); }
  Stats stats() const { return engines_.stats(); }

 private:
  ResourcePool<FigureClassifierEngine> engines_;
};

}  // namespace grparse
