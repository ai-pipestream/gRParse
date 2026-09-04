#pragma once

#include <cstddef>
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

class PageSource {
 public:
  virtual ~PageSource() = default;
  virtual int page_count() const = 0;
  virtual std::optional<OcrPage> extract_digital_page(int page_number) const;
  virtual cv::Mat render_page(int page_number) const = 0;
  // The engine name a remote PDF backend reported through Probe; empty for
  // the in-process sources. Consensus mode names its vote legs with it.
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

// pdf_parser_slots caps how many pages of one PDF may be parsed or rendered
// concurrently; each slot owns an independent Poppler document over the same
// request bytes.  Size it to the render worker count.
std::shared_ptr<PageSource> open_in_memory_document(std::shared_ptr<const std::string> bytes, bool pdf,
                                                    size_t pdf_parser_slots = 1,
                                                    double render_dpi = kDefaultRenderDpi);

}  // namespace grparse
