#include "grparse/in_memory_document.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-image.h>
#include <poppler/cpp/poppler-page.h>
#include <poppler/cpp/poppler-page-renderer.h>

#include "grparse/resource_pool.h"

namespace grparse {
namespace {

// Born-digital coverage gate: skip OCR only when the native text layer looks real.
constexpr size_t kMinDigitalNonWhitespace = 32;
constexpr size_t kMinDigitalLines = 4;
constexpr double kMinDigitalVerticalCoverage = 0.12;
constexpr size_t kStrongDigitalNonWhitespace = 128;
constexpr double kRenderDpi = 200.0;
constexpr double kPdfUserSpaceDpi = 72.0;
constexpr double kRenderScale = kRenderDpi / kPdfUserSpaceDpi;

int scaled(double user_space_units) {
  return static_cast<int>(std::lround(user_space_units * kRenderScale));
}

// Poppler applies the page's intrinsic /Rotate to both the raster and the text
// list, but page_rect() still reports unrotated media geometry.  Without this
// swap, provenance boxes on a rotated page would not fit the page size we
// advertise (and would not match the raster the OCR path measures).
bool is_quarter_turn(const poppler::page& page) {
  const auto orientation = page.orientation();
  return orientation == poppler::page::landscape || orientation == poppler::page::seascape;
}

class PdfPageSource final : public PageSource {
 public:
  PdfPageSource(std::shared_ptr<const std::string> bytes, size_t parser_slots)
      : bytes_(std::move(bytes)),
        parsers_(std::max<size_t>(parser_slots, 1), [bytes = bytes_] { return load(*bytes); }) {
    // Parse once now so an unreadable document fails before any page is queued.
    auto parser = parsers_.acquire();
    pages_ = parser->pages();
    if (pages_ <= 0) throw InvalidDocument("PDF does not contain a renderable page");
  }

  int page_count() const override { return pages_; }

  std::optional<OcrPage> extract_digital_page(int page_number) const override {
    auto parser = parsers_.acquire();
    const std::unique_ptr<poppler::page> page = open_page(*parser, page_number);
    const poppler::rectf page_rect = page->page_rect();
    const bool quarter_turn = is_quarter_turn(*page);

    OcrPage result;
    result.width = scaled(quarter_turn ? page_rect.height() : page_rect.width());
    result.height = scaled(quarter_turn ? page_rect.width() : page_rect.height());
    result.source = OcrPage::Source::kDigitalPdf;

    const double page_extent = quarter_turn ? page_rect.width() : page_rect.height();
    size_t non_whitespace_bytes = 0;
    double text_top = page_extent;
    double text_bottom = 0.0;
    const std::vector<poppler::text_box> text_boxes = page->text_list();
    result.lines.reserve(text_boxes.size());
    for (const auto& text_box : text_boxes) {
      const auto utf8 = text_box.text().to_utf8();
      std::string text(utf8.begin(), utf8.end());
      if (text.empty()) continue;
      const poppler::rectf box = text_box.bbox();
      for (const unsigned char byte : text) {
        if (std::isspace(byte) == 0) ++non_whitespace_bytes;
      }
      text_top = std::min(text_top, box.top());
      text_bottom = std::max(text_bottom, box.bottom());
      const int left = scaled(box.left());
      const int top = scaled(box.top());
      const int right = scaled(box.right());
      const int bottom = scaled(box.bottom());
      result.lines.push_back(OcrLine{std::move(text),
                                     {{left, top}, {right, top}, {right, bottom}, {left, bottom}},
                                     std::nullopt,
                                     TextOrigin::kDigitalPdf});
    }
    if (result.lines.empty()) return std::nullopt;

    const double vertical_coverage =
        page_extent > 0.0 ? (text_bottom - text_top) / page_extent : 0.0;
    result.skip_ocr = non_whitespace_bytes >= kMinDigitalNonWhitespace &&
                      result.lines.size() >= kMinDigitalLines &&
                      (vertical_coverage >= kMinDigitalVerticalCoverage ||
                       non_whitespace_bytes >= kStrongDigitalNonWhitespace);
    // Always return native text when present so the scheduler can merge with OCR on weak pages.
    return result;
  }

  cv::Mat render_page(int page_number) const override {
    auto parser = parsers_.acquire();
    const std::unique_ptr<poppler::page> page = open_page(*parser, page_number);

    poppler::page_renderer renderer;
    renderer.set_image_format(poppler::image::format_bgr24);
    const poppler::image image = renderer.render_page(page.get(), kRenderDpi, kRenderDpi);
    if (!image.is_valid()) throw InvalidDocument("PDF page could not be rendered in memory");
    // Poppler owns image.const_data() for this stack frame only — clone before return.
    return cv::Mat(image.height(), image.width(), CV_8UC3, const_cast<char*>(image.const_data()),
                   static_cast<size_t>(image.bytes_per_row()))
        .clone();
  }

 private:
  // Each pooled parser is an independent poppler::document over the shared,
  // immutable request buffer, so render workers no longer serialise on one
  // document lock.  load_from_raw_data does not copy: the captured shared_ptr
  // keeps the bytes alive for as long as the pool holds documents.
  static std::unique_ptr<poppler::document> load(const std::string& bytes) {
    if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
      throw InvalidDocument("PDF exceeds the Poppler in-memory input limit");
    }
    auto* raw = poppler::document::load_from_raw_data(bytes.data(), static_cast<int>(bytes.size()));
    if (raw == nullptr) throw InvalidDocument("PDF could not be opened from memory");
    return std::unique_ptr<poppler::document>(raw);
  }

  std::unique_ptr<poppler::page> open_page(poppler::document& parser, int page_number) const {
    if (page_number < 1 || page_number > pages_) throw InvalidDocument("PDF page number is out of range");
    std::unique_ptr<poppler::page> page(parser.create_page(page_number - 1));
    if (!page) throw InvalidDocument("PDF page could not be opened");
    return page;
  }

  std::shared_ptr<const std::string> bytes_;
  mutable ResourcePool<poppler::document> parsers_;
  int pages_ = 0;
};

class RasterPageSource final : public PageSource {
 public:
  explicit RasterPageSource(std::shared_ptr<const std::string> bytes) : bytes_(std::move(bytes)) {
    if (bytes_->size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
      throw InvalidDocument("Raster image exceeds the in-memory decode limit");
    }
  }

  int page_count() const override { return 1; }

  cv::Mat render_page(int page_number) const override {
    if (page_number != 1) throw InvalidDocument("Raster page number is out of range");
    // Decode from the existing buffer without an intermediate std::vector copy.
    const cv::Mat encoded(1, static_cast<int>(bytes_->size()), CV_8UC1,
                          const_cast<char*>(bytes_->data()));
    cv::Mat image = cv::imdecode(encoded, cv::IMREAD_COLOR);
    if (image.empty()) throw InvalidDocument("Raster image could not be decoded from memory");
    return image;
  }

 private:
  std::shared_ptr<const std::string> bytes_;
};

}  // namespace

std::optional<OcrPage> PageSource::extract_digital_page(int) const { return std::nullopt; }

std::shared_ptr<PageSource> open_in_memory_document(std::shared_ptr<const std::string> bytes, bool pdf,
                                                    size_t pdf_parser_slots) {
  if (!bytes || bytes->empty()) throw InvalidDocument("Document bytes are empty");
  if (pdf) return std::make_shared<PdfPageSource>(std::move(bytes), pdf_parser_slots);
  return std::make_shared<RasterPageSource>(std::move(bytes));
}

}  // namespace grparse
