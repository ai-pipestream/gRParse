#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"

namespace grparse {

class InvalidDocument final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
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
};

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
