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
};

using PageSourceFactory =
    std::function<std::shared_ptr<PageSource>(std::shared_ptr<const std::string> bytes, bool pdf)>;

// pdf_parser_slots caps how many pages of one PDF may be parsed or rendered
// concurrently; each slot owns an independent Poppler document over the same
// request bytes.  Size it to the render worker count.
std::shared_ptr<PageSource> open_in_memory_document(std::shared_ptr<const std::string> bytes, bool pdf,
                                                    size_t pdf_parser_slots = 1);

}  // namespace grparse
