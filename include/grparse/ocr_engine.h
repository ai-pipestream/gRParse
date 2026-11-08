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

// The detector's input is dynamic, and every new input shape costs the
// accelerated providers a fresh cuDNN algorithm choice or OpenVINO kernel
// compile.  So the longest side of the detector input is taken from a short
// ladder of sizes (kOcrDetectorLadder, then the cap) instead of following
// each page, and the page is letterboxed onto it: scaled to that size on
// both axes alike, with the short side padded with white to a multiple of
// 32.  Pages of one shape then share one detector input whatever size they
// were rendered at.  A page smaller than the ladder's first size is padded
// out to it instead of scaled up.
inline constexpr int kOcrDetectorLadder[] = {960,  1280, 1600, 2048,  2560,  3200, 4096,
                                             5120, 6400, 8192, 10240, 12800, 16384};

// The ladder size a page whose padded longest side is `longest_side` is
// detected at: the size nearest to it by ratio (the larger on a tie), or the
// smallest size when the page is smaller still.  Sizes above `max_side_len`
// are not on the ladder; the cap itself, rounded down to a multiple of 32,
// takes the top.  The result never exceeds max_side_len and is always a
// multiple of 32, at least 32.  Below the cap, rounding to the nearer size
// bounds the change either way: a page is scaled down by at most about 13%
// or up by at most about 15%.
int ocr_detector_side(int longest_side, int max_side_len);

// Where one page goes on its way into the detector.  The canvas is the page
// with `padding` white pixels on every side and, where the letterbox needs
// it, more white on the right and bottom, in page pixels; the input is the
// detector tensor the canvas is resized to, both sides multiples of 32 and
// the longest exactly ocr_detector_side().  The canvas is never smaller
// than the padded page, and the resize from canvas to input scales both
// axes alike to within a pixel.
struct OcrDetectGeometry {
  int canvas_width = 0;
  int canvas_height = 0;
  int input_width = 0;
  int input_height = 0;
};
OcrDetectGeometry ocr_detect_geometry(int page_width, int page_height, int padding,
                                      int max_side_len);

class OcrEngine final {
 public:
  using Line = OcrLine;
  using Page = OcrPage;

  OcrEngine(const std::filesystem::path& model_directory, int gpu_index);

  Page extract_page(const cv::Mat& image);
  static constexpr const char* execution_provider() { return "CUDA"; }

 private:
  std::unique_ptr<OcrLite> engine_;
  // First inference compiles provider kernels; see the gate in
  // extract_page() (src/ocr_engine.cpp).
  bool warmed_ = false;
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
  using Stats = ResourcePool<OcrEngine>::Stats;

  OcrEnginePool(const std::filesystem::path& model_directory, size_t worker_count, int gpu_index);

  Lease acquire() { return engines_.acquire(); }
  OcrPage extract_page(const cv::Mat& image) override;
  size_t size() const { return engines_.capacity(); }
  // wait_ns rising faster than pages complete means inference workers outnumber
  // warm sessions; discards count sessions rebuilt after a device error.
  Stats stats() const { return engines_.stats(); }

 private:
  ResourcePool<OcrEngine> engines_;
};

}  // namespace grparse
