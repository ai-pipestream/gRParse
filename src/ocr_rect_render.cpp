#include "grparse/ocr_rect_render.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace grparse {

OcrRectRenderer::OcrRectRenderer(Opener open, double base_dpi, double ocr_dpi)
    : open_(std::move(open)), scale_(base_dpi > 0.0 ? ocr_dpi / base_dpi : 1.0) {}

cv::Mat OcrRectRenderer::render(int page_number, std::string* failure) {
  std::shared_ptr<PageSource> source;
  std::string why;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_) return {};
    if (!opened_) {
      opened_ = true;
      try {
        source_ = open_ ? open_() : nullptr;
        if (source_ == nullptr) open_failure_ = "the document did not open at the OCR DPI";
      } catch (const std::exception& error) {
        source_ = nullptr;
        open_failure_ = std::string("the document did not open at the OCR DPI: ") + error.what();
      }
    }
    source = source_;
    why = open_failure_;
  }
  if (source != nullptr) {
    try {
      cv::Mat image = source->render_page(page_number);
      if (!image.empty()) return image;
      why = "the render at the OCR DPI came back empty";
    } catch (const std::exception& error) {
      why = std::string("the render at the OCR DPI failed: ") + error.what();
    }
  }
  // A cancel that landed while the render was in flight aborted the backend
  // call, which then failed: that is the request ending, not a weaker read
  // for the caller to report, so the reason is not handed on.
  const std::lock_guard<std::mutex> lock(mutex_);
  if (failure != nullptr && !cancelled_) *failure = std::move(why);
  return {};
}

void OcrRectRenderer::cancel() {
  std::shared_ptr<PageSource> source;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    source = source_;
  }
  if (source != nullptr) source->cancel();
}

std::shared_ptr<OcrRectRenderer> make_ocr_rect_renderer(
    PageSourceFactory factory, std::shared_ptr<const std::string> bytes, bool pdf, double base_dpi,
    double ocr_dpi, std::string sha256_hex, std::chrono::system_clock::time_point deadline,
    DocumentPasswords passwords) {
  if (!pdf || !factory || ocr_dpi <= base_dpi) return nullptr;
  auto open = [factory = std::move(factory), bytes = std::move(bytes), ocr_dpi, sha = std::move(sha256_hex),
               deadline, passwords = std::move(passwords)] {
    auto source = factory(bytes, true, ocr_dpi, sha);
    if (source) {
      source->set_deadline(deadline);
      source->set_passwords(passwords);
    }
    return source;
  };
  return std::make_shared<OcrRectRenderer>(std::move(open), base_dpi, ocr_dpi);
}

cv::Rect scaled_rect(const cv::Rect& rect, double scale, cv::Size size) {
  const int left = std::clamp(static_cast<int>(std::lround(rect.x * scale)), 0, size.width);
  const int top = std::clamp(static_cast<int>(std::lround(rect.y * scale)), 0, size.height);
  const int right = std::clamp(static_cast<int>(std::lround((rect.x + rect.width) * scale)), 0, size.width);
  const int bottom = std::clamp(static_cast<int>(std::lround((rect.y + rect.height) * scale)), 0, size.height);
  return {left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

void scale_read(OcrPage* read, double scale) {
  if (read == nullptr || scale <= 0.0) return;
  for (auto& line : read->lines) {
    for (auto& point : line.polygon) {
      point.x = static_cast<int>(std::lround(point.x / scale));
      point.y = static_cast<int>(std::lround(point.y / scale));
    }
  }
}

}  // namespace grparse
