#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"
#include "grparse/resource_pool.h"

namespace grparse {

// Detects layout regions on a rendered page raster.  The scheduler talks to
// this interface so tests can label pages without a model.
class RegionDetector {
 public:
  virtual ~RegionDetector() = default;
  virtual std::vector<LayoutRegion> detect_regions(const cv::Mat& image) = 0;
};

// PicoDet layout detector (PP-StructureV2 PubLayNet export) on ONNX Runtime.
// The session binds the process-wide execution provider selection, so CUDA
// and OpenVINO builds pool layout sessions exactly like OCR sessions.
//
// Anti-seesaw contract: detect_regions is a batch=1 device call on the same
// raster the OCR stage already holds; it neither retains the image nor blocks
// on anything but its own inference.
class LayoutEngine final {
 public:
  // PubLayNet label set, index == model class id.
  static const std::vector<std::string>& labels();

  // Throws when the model is missing or the configured execution provider
  // cannot initialize (startup fail-loud, same policy as OCR).
  explicit LayoutEngine(const std::filesystem::path& model_path);
  ~LayoutEngine();
  LayoutEngine(const LayoutEngine&) = delete;
  LayoutEngine& operator=(const LayoutEngine&) = delete;

  // BGR image in, regions in that image's pixel space out.
  std::vector<LayoutRegion> detect_regions(const cv::Mat& image);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Warm exclusive-lease pool over LayoutEngine, one session per slot.
class LayoutEnginePool final : public RegionDetector {
 public:
  using Stats = ResourcePool<LayoutEngine>::Stats;

  // Builds every session eagerly so a bad model or provider fails startup.
  LayoutEnginePool(const std::filesystem::path& model_path, size_t worker_count);

  std::vector<LayoutRegion> detect_regions(const cv::Mat& image) override;
  size_t size() const { return engines_.capacity(); }
  Stats stats() const { return engines_.stats(); }

 private:
  ResourcePool<LayoutEngine> engines_;
};

}  // namespace grparse
