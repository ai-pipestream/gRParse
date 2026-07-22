#include "grparse/in_memory_document.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-image.h>
#include <poppler/cpp/poppler-page.h>
#include <poppler/cpp/poppler-page-renderer.h>

namespace grparse {
namespace {

class PdfPageSource final : public PageSource {
 public:
  explicit PdfPageSource(std::shared_ptr<const std::string> bytes) : bytes_(std::move(bytes)), document_(load()) {
    pages_ = document_->pages();
    if (pages_ <= 0) throw InvalidDocument("PDF does not contain a renderable page");
  }

  int page_count() const override { return pages_; }

  std::optional<OcrPage> extract_digital_page(int page_number) const override {
    std::lock_guard<std::mutex> lock(document_mutex_);
    auto page = open_page(page_number);
    const poppler::rectf page_rect = page->page_rect();
    constexpr double kScale = 200.0 / 72.0;
    OcrPage result{static_cast<int>(page_rect.width() * kScale),
                   static_cast<int>(page_rect.height() * kScale), {}};
    result.source = OcrPage::Source::kDigitalPdf;
    size_t non_whitespace_bytes = 0;
    double text_top = page_rect.height();
    double text_bottom = 0.0;
    for (const auto& text_box : page->text_list()) {
      const auto utf8 = text_box.text().to_utf8();
      std::string text(utf8.begin(), utf8.end());
      if (text.empty()) continue;
      const poppler::rectf box = text_box.bbox();
      for (const unsigned char byte : text) {
        if (!std::isspace(byte)) ++non_whitespace_bytes;
      }
      text_top = std::min(text_top, box.top());
      text_bottom = std::max(text_bottom, box.bottom());
      const int left = static_cast<int>(box.left() * kScale);
      const int top = static_cast<int>(box.top() * kScale);
      const int right = static_cast<int>(box.right() * kScale);
      const int bottom = static_cast<int>(box.bottom() * kScale);
      result.lines.push_back(
          OcrLine{std::move(text), {{left, top}, {right, top}, {right, bottom}, {left, bottom}},
                  std::nullopt});
    }
    if (result.lines.empty()) return std::nullopt;
    const double vertical_coverage =
        page_rect.height() > 0.0 ? (text_bottom - text_top) / page_rect.height() : 0.0;
    const bool sufficient_text = non_whitespace_bytes >= 32 && result.lines.size() >= 4 &&
                                 (vertical_coverage >= 0.12 || non_whitespace_bytes >= 128);
    if (!sufficient_text) return std::nullopt;
    return result;
  }

  cv::Mat render_page(int page_number) const override {
    std::lock_guard<std::mutex> lock(document_mutex_);
    auto page = open_page(page_number);

    poppler::page_renderer renderer;
    renderer.set_image_format(poppler::image::format_bgr24);
    const poppler::image image = renderer.render_page(page.get(), 200.0, 200.0);
    if (!image.is_valid()) throw InvalidDocument("PDF page could not be rendered in memory");
    return cv::Mat(image.height(), image.width(), CV_8UC3, const_cast<char*>(image.const_data()),
                   static_cast<size_t>(image.bytes_per_row()))
        .clone();
  }

 private:
  std::unique_ptr<poppler::document> load() const {
    if (bytes_->size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
      throw InvalidDocument("PDF exceeds the Poppler in-memory input limit");
    }
    auto* raw = poppler::document::load_from_raw_data(bytes_->data(), static_cast<int>(bytes_->size()));
    if (raw == nullptr) throw InvalidDocument("PDF could not be opened from memory");
    return std::unique_ptr<poppler::document>(raw);
  }

  std::unique_ptr<poppler::page> open_page(int page_number) const {
    if (page_number < 1 || page_number > pages_) throw InvalidDocument("PDF page number is out of range");
    std::unique_ptr<poppler::page> page(document_->create_page(page_number - 1));
    if (!page) throw InvalidDocument("PDF page could not be opened");
    return page;
  }

  std::shared_ptr<const std::string> bytes_;
  std::unique_ptr<poppler::document> document_;
  mutable std::mutex document_mutex_;
  int pages_ = 0;
};

class RasterPageSource final : public PageSource {
 public:
  explicit RasterPageSource(std::shared_ptr<const std::string> bytes) : bytes_(std::move(bytes)) {}

  int page_count() const override { return 1; }

  cv::Mat render_page(int page_number) const override {
    if (page_number != 1) throw InvalidDocument("Raster page number is out of range");
    const auto* begin = reinterpret_cast<const unsigned char*>(bytes_->data());
    const std::vector<unsigned char> encoded(begin, begin + bytes_->size());
    const cv::Mat image = cv::imdecode(encoded, cv::IMREAD_COLOR);
    if (image.empty()) throw InvalidDocument("Raster image could not be decoded from memory");
    return image;
  }

 private:
  std::shared_ptr<const std::string> bytes_;
};

}  // namespace

std::optional<OcrPage> PageSource::extract_digital_page(int) const { return std::nullopt; }

std::shared_ptr<PageSource> open_in_memory_document(std::shared_ptr<const std::string> bytes, bool pdf) {
  if (!bytes || bytes->empty()) throw InvalidDocument("Document bytes are empty");
  if (pdf) return std::make_shared<PdfPageSource>(std::move(bytes));
  return std::make_shared<RasterPageSource>(std::move(bytes));
}

}  // namespace grparse
