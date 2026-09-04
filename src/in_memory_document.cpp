#include "grparse/in_memory_document.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
// Resolved via pkg-config's poppler-cpp Cflags (includedir ends in
// poppler/cpp), which is what lets the vendored /opt/poppler prefix win over
// any distro headers; the old <poppler/cpp/...> spelling bypassed the .pc
// contract and only worked off the default /usr/include path.
#include <poppler-document.h>
#include <poppler-image.h>
#include <poppler-page.h>
#include <poppler-page-renderer.h>

#include "grparse/consensus_page_source.h"
#include "grparse/remote_page_source.h"
#include "grparse/resource_pool.h"

namespace grparse {
namespace {

// Born-digital coverage gate: skip OCR only when the native text layer looks real.
constexpr size_t kMinDigitalNonWhitespace = 32;
constexpr size_t kMinDigitalLines = 4;
constexpr double kMinDigitalVerticalCoverage = 0.12;
constexpr size_t kStrongDigitalNonWhitespace = 128;
constexpr double kPdfUserSpaceDpi = 72.0;

// Poppler applies the page's intrinsic /Rotate to both the raster and the text
// list, but page_rect() still reports unrotated media geometry.  Without this
// swap, provenance boxes on a rotated page would not fit the page size we
// advertise (and would not match the raster the OCR path measures).
bool is_quarter_turn(const poppler::page& page) {
  const auto orientation = page.orientation();
  return orientation == poppler::page::landscape || orientation == poppler::page::seascape;
}

// Poppler's documented threading contract (one document per thread) is not
// enough on arm64: concurrent work on a PDF whose broken fonts hammer the
// substitute-font fallback corrupts shared state even across fully
// independent documents (std::system_error or SIGSEGV out of XRef::fetch
// from two threads at once - AcroForm sheets with /DA fonts missing from
// /DR are the trigger). VERIFIED STILL BROKEN IN POPPLER 26.08: two-thread
// hammer runs on real arm64 hardware crash both 26.01 and 26.08 at a
// roughly 5-12% rate per run (SIGSEGV on Cortex-A76, SIGABRT on
// Cortex-A78), so the 26.06 annots use-after-free fixes (4aca25d6,
// 2f10803d) are adjacent but not this bug. x86-64 has never reproduced it
// under the same hammering - its stronger memory ordering hides whatever
// the race is - so the serialisation stays exactly where the crash exists
// and costs the primary x86-64 targets nothing. This gate is load-bearing
// on arm64: do not remove it on a poppler version bump without re-running
// the two-thread repro on real arm64 silicon first.
#if defined(__aarch64__)
class PopplerGate {
 public:
  PopplerGate() : lock_(mutex()) {}

 private:
  static std::mutex& mutex() {
    static std::mutex mutex;
    return mutex;
  }
  const std::lock_guard<std::mutex> lock_;
};
#else
class PopplerGate {};
#endif

class PdfPageSource final : public PageSource {
 public:
  PdfPageSource(std::shared_ptr<const std::string> bytes, size_t parser_slots, double render_dpi)
      : bytes_(std::move(bytes)),
        render_dpi_(render_dpi),
        render_scale_(render_dpi / kPdfUserSpaceDpi),
        parsers_(std::max<size_t>(parser_slots, 1), [bytes = bytes_] { return load(*bytes); }) {
    // Parse once now so an unreadable document fails before any page is queued.
    [[maybe_unused]] const PopplerGate poppler_gate;
    auto parser = parsers_.acquire();
    pages_ = parser->pages();
    if (pages_ <= 0) throw InvalidDocument("PDF does not contain a renderable page");
  }

  int page_count() const override { return pages_; }

  std::optional<OcrPage> extract_digital_page(int page_number) const override {
    // The poppler section ends when this scope does - the boxes own their
    // strings and rects, so the fold below runs unserialised. The page is
    // destroyed inside the scope, before any gate releases.
    poppler::rectf page_rect;
    bool quarter_turn = false;
    std::vector<poppler::text_box> text_boxes;
    {
      [[maybe_unused]] const PopplerGate poppler_gate;
      auto parser = parsers_.acquire();
      const std::unique_ptr<poppler::page> page = open_page(*parser, page_number);
      page_rect = page->page_rect();
      quarter_turn = is_quarter_turn(*page);
      // Font info is one flag away and is the only heading-depth signal a
      // born-digital page has that survives without rasterizing.
      text_boxes = page->text_list(poppler::page::text_list_include_font);
    }

    OcrPage result;
    result.width = scaled(quarter_turn ? page_rect.height() : page_rect.width());
    result.height = scaled(quarter_turn ? page_rect.width() : page_rect.height());
    result.source = OcrPage::Source::kDigitalPdf;

    const double page_extent = quarter_turn ? page_rect.width() : page_rect.height();
    size_t non_whitespace_bytes = 0;
    double text_top = page_extent;
    double text_bottom = 0.0;
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
      OcrLine line{std::move(text),
                   {{left, top}, {right, top}, {right, bottom}, {left, bottom}},
                   std::nullopt,
                   TextOrigin::kDigitalPdf};
      if (text_box.has_font_info()) {
        std::string font = text_box.get_font_name();
        // Embedded subsets carry a random six-letter prefix (ABCDEF+Real).
        if (const auto plus = font.find('+');
            plus != std::string::npos && plus + 1 < font.size()) {
          font.erase(0, plus + 1);
        }
        if (!font.empty()) line.font_name = std::move(font);
        const double size = text_box.get_font_size();
        if (size > 0) line.font_size_pt = size;
      }
      result.lines.push_back(std::move(line));
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
    // Gated end to end: rendering drives the same font machinery, and the
    // clone must happen while poppler still owns the image buffer.
    [[maybe_unused]] const PopplerGate poppler_gate;
    auto parser = parsers_.acquire();
    const std::unique_ptr<poppler::page> page = open_page(*parser, page_number);

    poppler::page_renderer renderer;
    renderer.set_image_format(poppler::image::format_bgr24);
    const poppler::image image = renderer.render_page(page.get(), render_dpi_, render_dpi_);
    if (!image.is_valid()) throw InvalidDocument("PDF page could not be rendered in memory");
    // Poppler owns image.const_data() for this stack frame only — clone before return.
    return cv::Mat(image.height(), image.width(), CV_8UC3, const_cast<char*>(image.const_data()),
                   static_cast<size_t>(image.bytes_per_row()))
        .clone();
  }

 private:
  // Digital-line geometry scales from PDF user space into the same pixel
  // space render_page produces, so a per-document DPI stays self-consistent.
  int scaled(double user_space_units) const {
    return static_cast<int>(std::lround(user_space_units * render_scale_));
  }

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
  const double render_dpi_;
  const double render_scale_;
  mutable ResourcePool<poppler::document> parsers_;
  int pages_ = 0;
};

// The service advertises PNG, JPEG, and TIFF raster input.  cv::imdecode,
// left to sniff on its own, accepts far more - WebP, BMP, and, where the
// build links GDAL, JPEG2000/NITF/GeoTIFF, some of which spill the buffer to
// a temp file (breaking the diskless guarantee) and can leak descriptors on
// malformed input.  Gate on the container magic before imdecode ever runs so
// only the three advertised formats reach a decoder.
bool is_supported_raster(const std::string& bytes) {
  auto starts_with = [&bytes](std::initializer_list<unsigned char> magic) {
    if (bytes.size() < magic.size()) return false;
    size_t index = 0;
    for (const unsigned char expected : magic) {
      if (static_cast<unsigned char>(bytes[index++]) != expected) return false;
    }
    return true;
  };
  return starts_with({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}) ||  // PNG
         starts_with({0xFF, 0xD8, 0xFF}) ||                                // JPEG
         starts_with({0x49, 0x49, 0x2A, 0x00}) ||                          // TIFF little-endian
         starts_with({0x4D, 0x4D, 0x00, 0x2A});                            // TIFF big-endian
}

class RasterPageSource final : public PageSource {
 public:
  explicit RasterPageSource(std::shared_ptr<const std::string> bytes) : bytes_(std::move(bytes)) {
    if (bytes_->size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
      throw InvalidDocument("Raster image exceeds the in-memory decode limit");
    }
    if (!is_supported_raster(*bytes_)) {
      throw InvalidDocument("Raster image is not a supported format (PNG, JPEG, or TIFF)");
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
                                                    size_t pdf_parser_slots, double render_dpi) {
  if (!bytes || bytes->empty()) throw InvalidDocument("Document bytes are empty");
  if (!(render_dpi > 0.0)) throw std::invalid_argument("Render DPI must be positive");
  if (pdf) {
    // GRPARSE_PDF_BACKEND selects PdfBackendService targets for the PDF
    // layer; unset or "inprocess" keeps the poppler path below. A single
    // target dials that backend; a comma-separated list reads the document
    // through every backend and takes each page from the one whose word
    // order wins the consensus vote.
    if (const auto target = remote_pdf_backend_target()) {
      const auto targets = split_backend_targets(*target);
      if (targets.size() > 1) {
        return open_consensus_pdf_document(std::move(bytes), targets, render_dpi);
      }
      return open_remote_pdf_document(std::move(bytes), *target, render_dpi);
    }
    return std::make_shared<PdfPageSource>(std::move(bytes), pdf_parser_slots, render_dpi);
  }
  return std::make_shared<RasterPageSource>(std::move(bytes));
}

}  // namespace grparse
