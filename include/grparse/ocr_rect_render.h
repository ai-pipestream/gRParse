#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <opencv2/core.hpp>

#include "grparse/document_passwords.h"
#include "grparse/in_memory_document.h"
#include "grparse/ocr_types.h"

namespace grparse {

// The DPI image rectangles on a text-layer page are OCRed at, when the page
// raster (and the layout model) stays at the render DPI. Small text drawn
// into a raster figure (axis labels, screenshots) reads better sharper: on
// 101 ACM papers a full-page 300 DPI render recovered about 1,850 more
// figure words than 200 DPI, but moved the layout boxes; rendering only the
// pages with OCR rectangles a second time keeps layout where it was.
inline constexpr double kDefaultOcrImageDpi = 300.0;

// A second view of one document at the OCR DPI, opened on first use, shared
// by the document's pages and safe to call from several inference threads.
class OcrRectRenderer {
 public:
  using Opener = std::function<std::shared_ptr<PageSource>()>;

  // `open` returns the document's page source at `ocr_dpi`; it runs once, on
  // the first render. `base_dpi` is the DPI of the page raster the OCR
  // rectangles are measured in.
  OcrRectRenderer(Opener open, double base_dpi, double ocr_dpi);

  // The page at the OCR DPI, or an empty Mat when the source does not open,
  // the render fails, or the renderer was cancelled. The caller reads the
  // page raster instead of an empty Mat, a weaker read it has to own up to:
  // `failure`, when given, receives why the source did not open or the
  // render failed. It is left as it was on success and after a cancel,
  // which ends the request and leaves no read to report.
  cv::Mat render(int page_number, std::string* failure = nullptr);

  // The OCR DPI over the base DPI.
  double scale() const { return scale_; }

  // Aborts the second source's backend calls with the request's own.
  void cancel();

 private:
  Opener open_;
  double scale_;
  std::mutex mutex_;
  bool opened_ = false;
  bool cancelled_ = false;
  std::shared_ptr<PageSource> source_;
  // Why the source did not open; the open runs once, so every later render
  // fails for the same reason and says so.
  std::string open_failure_;
};

// The renderer for one document: its PDF reopened through `factory` at
// `ocr_dpi` with the request's deadline and passwords. Null for a raster
// input or when `ocr_dpi` is not above `base_dpi` (the rectangles are then
// read from the page raster).
std::shared_ptr<OcrRectRenderer> make_ocr_rect_renderer(
    PageSourceFactory factory, std::shared_ptr<const std::string> bytes, bool pdf, double base_dpi,
    double ocr_dpi, std::string sha256_hex, std::chrono::system_clock::time_point deadline,
    DocumentPasswords passwords);

// `rect`, measured on the base raster, on a raster `scale` times larger,
// clamped to `size`. Pure.
cv::Rect scaled_rect(const cv::Rect& rect, double scale, cv::Size size);

// Brings an OCR read of a scaled crop back to base-raster pixels: every
// polygon point divided by `scale` and rounded. Pure.
void scale_read(OcrPage* read, double scale);

}  // namespace grparse
