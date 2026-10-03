#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"

namespace grparse {

class InvalidDocument : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Why a PDF backend call failed when the document is not at fault. This
// header stays free of gRPC (the fuzz build compiles it without one); the
// service maps each reason to its gRPC code.
enum class PdfBackendFailure { kUnavailable, kDeadlineExceeded, kCancelled, kResourceExhausted };

// A PDF backend call failed for a reason that is not the document's fault:
// the backend was unreachable, the request's deadline passed, or the call
// was cancelled. It is still an InvalidDocument so consensus falls over to
// the next leg, but the service answers with the matching transport code
// instead of INVALID_ARGUMENT, so clients and dashboards can tell an outage
// from a bad file.
class PdfBackendUnavailable final : public InvalidDocument {
 public:
  PdfBackendUnavailable(const std::string& what, PdfBackendFailure reason)
      : InvalidDocument(what), reason_(reason) {}
  PdfBackendFailure reason() const { return reason_; }

 private:
  PdfBackendFailure reason_;
};

// A PDF arrived and GRPARSE_PDF_BACKEND names no PdfBackendService. gRParse
// links no PDF engine of its own (the engines run as separate services, so
// their licenses stay with their containers), so this is a deployment gap,
// not a bad document: the service maps it to FAILED_PRECONDITION, the way
// an unconfigured collector fails.
class PdfBackendNotConfigured final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class PageSource {
 public:
  virtual ~PageSource() = default;
  virtual int page_count() const = 0;
  virtual std::optional<OcrPage> extract_digital_page(int page_number) const;
  virtual cv::Mat render_page(int page_number) const = 0;
  // The engine name a PDF backend reported through Probe; empty for
  // rasters. Consensus mode names its vote legs with it.
  virtual std::string backend_name() const { return {}; }
  // Ties the source's backend calls to the request reading the document: no
  // call runs past `deadline`, and cancel() aborts the calls in flight and
  // fails every later one fast. Safe from any thread. Sources that make no
  // remote calls ignore both.
  virtual void set_deadline(std::chrono::system_clock::time_point) {}
  virtual void cancel() {}
};

// The largest raster page, in pixels, an image input may decode to:
// GRPARSE_MAX_IMAGE_PIXELS, default 200 megapixels (about 600 MB of BGR).
// Checked against the image header before any decoder allocates, so a
// small compressed file cannot inflate to gigabytes. Read per image; a
// malformed value raises std::invalid_argument, and the server also reads
// it at startup so the mistake fails there.
inline constexpr uint64_t kDefaultMaxImagePixels = 200'000'000;
// Most pages a multi-page TIFF may declare.
inline constexpr size_t kMaxRasterPages = 4096;
uint64_t max_image_pixels();

// Whether an encoded image's own header (PNG, JPEG, TIFF, GIF, BMP, or
// WebP) states a size within max_image_pixels() on every page. False when
// the header cannot be read, so a caller decoding embedded or collector
// images refuses what it cannot bound before cv::imdecode allocates.
bool encoded_image_within_pixel_cap(const std::string& bytes);

// The rasterization DPI a source uses when no per-document value arrives.
inline constexpr double kDefaultRenderDpi = 200.0;

// render_dpi is the per-document rasterization DPI; every page of the source
// renders at it and all digital-line geometry scales to match, so downstream
// coordinates stay self-consistent.  Raster sources are already pixels and
// ignore it.
using PageSourceFactory = std::function<std::shared_ptr<PageSource>(
    std::shared_ptr<const std::string> bytes, bool pdf, double render_dpi)>;

// Rasters (PNG, JPEG, TIFF) decode in process. PDFs are read through the
// PdfBackendService targets GRPARSE_PDF_BACKEND names: one target dials that
// backend, a comma-separated list runs the consensus vote across all of
// them. With the variable unset or empty a PDF raises
// PdfBackendNotConfigured.
std::shared_ptr<PageSource> open_in_memory_document(std::shared_ptr<const std::string> bytes, bool pdf,
                                                    double render_dpi = kDefaultRenderDpi);

}  // namespace grparse
