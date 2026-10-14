#include "grparse/page_previews.h"

#include <algorithm>
#include <exception>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "grparse/document_assembly.h"
#include "grparse/in_memory_document.h"
#include "grparse/page_source_watch.h"

namespace grparse {

namespace {

// The preview needs about kPagePreviewMaxSide pixels down the long side of
// a letter page; rendering at this DPI lands there directly instead of
// rendering at the recognition DPI and throwing three quarters away.
constexpr double kPreviewRenderDpi = 100.0;

}  // namespace

cv::Mat preview_of(const cv::Mat& raster) {
  const int longest = std::max(raster.cols, raster.rows);
  if (longest <= kPagePreviewMaxSide) return raster;
  const double scale = static_cast<double>(kPagePreviewMaxSide) / longest;
  cv::Mat scaled;
  cv::resize(raster, scaled, cv::Size(), scale, scale, cv::INTER_AREA);
  return scaled;
}

PagePreviewSession::PagePreviewSession(
    std::shared_ptr<const std::string> bytes, std::function<bool()> stop,
    std::chrono::system_clock::time_point deadline, DocumentPasswords passwords)
    : bytes_(std::move(bytes)),
      stop_(std::move(stop)),
      deadline_(deadline),
      passwords_(std::move(passwords)) {}

PagePreviewSession::~PagePreviewSession() = default;

bool PagePreviewSession::ensure_open() {
  if (open_attempted_) return open_succeeded_;
  open_attempted_ = true;
  if (bytes_ == nullptr) return false;
  try {
    // The opening Probe waits for the watch below, so it honors the request.
    source_ = open_in_memory_document(std::move(bytes_), /*pdf=*/true,
                                      kPreviewRenderDpi,
                                      SourceOpening::kOnFirstUse);
  } catch (const std::exception&) {
    return false;
  }
  if (!source_) return false;
  if (!passwords_.empty()) source_->set_passwords(passwords_);
  // A backend call that hangs (the opening Probe or a render) ends with
  // the request, not its own timeout.
  watch_ = std::make_unique<PageSourceWatch>(source_, deadline_, stop_);
  try {
    page_count_ = source_->page_count();
  } catch (const std::exception&) {
    return false;
  }
  open_succeeded_ = page_count_ > 0;
  return open_succeeded_;
}

void PagePreviewSession::attach(
    ai::pipestream::document::v1::Document* document,
    std::optional<std::pair<int, int>> page_range) {
  if (document == nullptr || !ensure_open()) return;
  int first = 1;
  int last = page_count_;
  if (page_range.has_value()) {
    first = std::max(first, page_range->first);
    last = std::min(last, page_range->second);
  }
  for (int page_no = first; page_no <= last; ++page_no) {
    if (stop_ && stop_()) return;
    cv::Mat raster;
    try {
      raster = source_->render_page(page_no);
    } catch (const std::exception&) {
      continue;
    }
    if (raster.empty()) continue;
    std::vector<unsigned char> png;
    if (!cv::imencode(".png", preview_of(raster), png, kPngEncodeParams)) continue;
    auto& page = (*document->mutable_pages())[page_no];
    page.set_page_no(page_no);
    set_picture_image(png, page.mutable_image());
  }
}

void attach_page_previews(std::shared_ptr<const std::string> bytes,
                          ai::pipestream::document::v1::Document* document,
                          std::optional<std::pair<int, int>> page_range,
                          const std::function<bool()>& stop,
                          std::chrono::system_clock::time_point deadline) {
  if (document == nullptr || bytes == nullptr) return;
  PagePreviewSession session(std::move(bytes), stop, deadline);
  session.attach(document, page_range);
}

}  // namespace grparse
