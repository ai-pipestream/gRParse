#pragma once

#include <filesystem>
#include <memory>

#include "OcrLite.h"
#include "grparse/ocr_types.h"
#include "grparse/resource_pool.h"

namespace grparse {

// Detection knobs read once from the environment at process start.  They are
// process-global on purpose: every engine in the pool must score identically.
struct OcrDetectOptions {
  int padding = 50;
  int max_side_len = 2048;
  float box_score_thresh = 0.6F;
  float box_thresh = 0.3F;
  float un_clip_ratio = 2.0F;
  bool do_angle = true;
  bool most_angle = true;
};

// Throws std::invalid_argument for a malformed GRPARSE_OCR_* value.
const OcrDetectOptions& ocr_detect_options();

class OcrEngine final {
 public:
  using Line = OcrLine;
  using Page = OcrPage;

  OcrEngine(const std::filesystem::path& model_directory, int gpu_index);

  Page extract_page(const cv::Mat& image);
  static constexpr const char* execution_provider() { return "CUDA"; }

 private:
  std::unique_ptr<OcrLite> engine_;
};

class PageRecognizer {
 public:
  virtual ~PageRecognizer() = default;
  virtual OcrPage extract_page(const cv::Mat& image) = 0;
};

// One warm ORT session per worker.  Sessions are built eagerly so a missing
// model or a dead execution provider fails the process at startup.
class OcrEnginePool final : public PageRecognizer {
 public:
  using Lease = ResourcePool<OcrEngine>::Lease;

  OcrEnginePool(const std::filesystem::path& model_directory, size_t worker_count, int gpu_index);

  Lease acquire() { return engines_.acquire(); }
  OcrPage extract_page(const cv::Mat& image) override;
  size_t size() const { return engines_.capacity(); }

 private:
  ResourcePool<OcrEngine> engines_;
};

}  // namespace grparse
