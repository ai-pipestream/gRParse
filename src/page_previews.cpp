#include "grparse/page_previews.h"

#include <algorithm>
#include <exception>
#include <string>
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
    std::chrono::system_clock::time_point deadline, DocumentPasswords passwords,
    std::shared_ptr<CancelRegistry> cancel_registry)
    : bytes_(std::move(bytes)),
      stop_(std::move(stop)),
      deadline_(deadline),
      passwords_(std::move(passwords)),
      cancel_registry_(std::move(cancel_registry)) {}

PagePreviewSession::~PagePreviewSession() = default;

bool PagePreviewSession::stopped() const { return stop_ && stop_(); }

bool PagePreviewSession::ensure_open() {
  if (open_attempted_) return open_succeeded_;
  open_attempted_ = true;
  if (bytes_ == nullptr) {
    open_failure_ = "the document's bytes were not available";
    return false;
  }
  try {
    // The opening Probe waits for the watch below, so it honors the request.
    source_ = open_in_memory_document(std::move(bytes_), /*pdf=*/true,
                                      kPreviewRenderDpi,
                                      SourceOpening::kOnFirstUse);
    if (!passwords_.empty()) source_->set_passwords(passwords_);
    // A backend call that hangs (the opening Probe or a render) ends with
    // the request, not its own timeout.
    watch_ = std::make_unique<PageSourceWatch>(source_, deadline_, stop_, cancel_registry_);
    page_count_ = source_->page_count();
  } catch (const std::exception& error) {
    // An open the request's own end cut short is not a preview failure:
    // the request's status says what happened to it.
    if (!stopped()) open_failure_ = error.what();
    return false;
  }
  open_succeeded_ = page_count_ > 0;
  if (!open_succeeded_) open_failure_ = "the PDF backend reported no pages";
  return open_succeeded_;
}

std::vector<Warning> PagePreviewSession::attach(
    ai::pipestream::document::v1::Document* document,
    std::optional<std::pair<int, int>> page_range) {
  namespace docv1 = ai::pipestream::document::v1;
  std::vector<Warning> warnings;
  if (document == nullptr) return warnings;
  if (!ensure_open()) {
    // Said once: the failure is the document's, and a stream that asks page
    // by page would otherwise repeat it for every page.
    if (open_failure_.has_value()) {
      warnings.push_back(Warning(docv1::WARNING_CODE_PAGE_PREVIEW_FAILED,
                                 "no page image could be rendered, the document did not open "
                                 "for rendering: " + *open_failure_));
      open_failure_.reset();
    }
    return warnings;
  }
  int first = 1;
  int last = page_count_;
  if (page_range.has_value()) {
    first = std::max(first, page_range->first);
    last = std::min(last, page_range->second);
  }
  const auto failed = [&warnings](int page_no, const std::string& why) {
    warnings.push_back(Warning(docv1::WARNING_CODE_PAGE_PREVIEW_FAILED,
                               "page " + std::to_string(page_no) +
                                   " has no page image: " + why)
                           .on_page(page_no));
  };
  for (int page_no = first; page_no <= last; ++page_no) {
    if (stopped()) return warnings;
    cv::Mat raster;
    try {
      raster = source_->render_page(page_no);
    } catch (const std::exception& error) {
      // A render the request's own end cut short is not a preview failure,
      // and neither are the pages after it.
      if (stopped()) return warnings;
      failed(page_no, error.what());
      continue;
    }
    if (raster.empty()) {
      failed(page_no, "the PDF backend returned an empty render");
      continue;
    }
    std::vector<unsigned char> png;
    if (!cv::imencode(".png", preview_of(raster), png, kPngEncodeParams)) {
      failed(page_no, "the render could not be encoded as PNG");
      continue;
    }
    auto& page = (*document->mutable_pages())[page_no];
    page.set_page_no(page_no);
    set_picture_image(png, page.mutable_image());
  }
  return warnings;
}

std::vector<Warning> attach_page_previews(std::shared_ptr<const std::string> bytes,
                                          ai::pipestream::document::v1::Document* document,
                                          std::optional<std::pair<int, int>> page_range,
                                          const std::function<bool()>& stop,
                                          std::chrono::system_clock::time_point deadline,
                                          DocumentPasswords passwords) {
  if (document == nullptr) return {};
  PagePreviewSession session(std::move(bytes), stop, deadline, std::move(passwords));
  return session.attach(document, page_range);
}

}  // namespace grparse
