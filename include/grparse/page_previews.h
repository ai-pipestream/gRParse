#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_passwords.h"
#include "grparse/warning.h"

namespace grparse {

class PageSource;
class PageSourceWatch;
class CancelRegistry;

// Longest side of a captured page preview. Rasters render at 200 DPI (a
// letter page is ~1700x2200); previews exist to be painted under boxes, not
// re-OCRed, so half that keeps events an order of magnitude smaller.
inline constexpr int kPagePreviewMaxSide = 1100;

// The PNG encoder setting every page and picture image is written with:
// zlib level 6, the level PIL writes at and the level docling-core pins its
// own OpenCV encodes to (2.96), so a preview's bytes match the reference
// pipeline's for the same raster.
inline const std::vector<int> kPngEncodeParams = {cv::IMWRITE_PNG_COMPRESSION, 6};

// Downscaled copy of the raster for the page preview; the raster itself when
// it is already within bounds. Never aliases past the encode that follows.
cv::Mat preview_of(const cv::Mat& raster);

// Renders page previews over a single lazily opened PageSource so a caller
// that streams a document page by page hashes and probes the PDF at most once
// instead of once per streamed page. A non-null `cancel_registry` pushes the
// request's cancel to the source instead of polling `stop` for it.
class PagePreviewSession {
 public:
  PagePreviewSession(
      std::shared_ptr<const std::string> bytes,
      std::function<bool()> stop = {},
      std::chrono::system_clock::time_point deadline =
          std::chrono::system_clock::time_point::max(),
      DocumentPasswords passwords = {},
      std::shared_ptr<CancelRegistry> cancel_registry = {});
  ~PagePreviewSession();
  PagePreviewSession(const PagePreviewSession&) = delete;
  PagePreviewSession& operator=(const PagePreviewSession&) = delete;

  // Attaches a PNG preview of each page in `page_range` (or all pages when
  // unset) to `document->mutable_pages()`. Opens and probes the underlying
  // PageSource on the first call.
  //
  // Returns what the caller asked for and did not get, as
  // WARNING_CODE_PAGE_PREVIEW_FAILED: one warning naming the page for each
  // page whose render or encode failed, or a single warning without a page
  // when the document could not be opened for rendering at all. That one is
  // said once per session, however many slices follow, since it is about
  // the document and not about any page of it. A request that is over
  // (`stop` answered true) reports nothing for the pages it left: its
  // status says what happened to it.
  [[nodiscard]] std::vector<Warning> attach(
      ai::pipestream::document::v1::Document* document,
      std::optional<std::pair<int, int>> page_range = std::nullopt);

 private:
  bool ensure_open();
  bool stopped() const;

  std::shared_ptr<const std::string> bytes_;
  std::function<bool()> stop_;
  std::chrono::system_clock::time_point deadline_;
  DocumentPasswords passwords_;
  std::shared_ptr<CancelRegistry> cancel_registry_;
  std::shared_ptr<PageSource> source_;
  std::unique_ptr<PageSourceWatch> watch_;
  int page_count_ = 0;
  bool open_attempted_ = false;
  bool open_succeeded_ = false;
  // Why the document did not open, until attach() has said so; unset when
  // it opened, when the request ended first, and once it has been reported.
  std::optional<std::string> open_failure_;
};

// Attaches a PNG preview of every page to Document.pages for a PDF a
// collector folded without rasterizing. The in-process CV path captures its
// previews off the rasters it renders anyway; an inspector-routed document
// never rendered, so the shell painted its boxes on a blank page. The
// previews are the same size class as the CV path's and ride the same field
// (PageItem.image), so a consumer cannot tell which path drew them.
//
// Pages the document names but the PDF lacks keep their entry untouched;
// pages the PDF has that the document never named are added with only their
// number and preview. Bytes that do not open as a PDF leave the document
// exactly as it was: a preview is an aid, never a reason to fail a parse.
// What could not be attached comes back as PagePreviewSession::attach
// returns it, so the caller that asked for page images is told which are
// missing instead of finding out from their absence.
//
// `page_range` (inclusive, 1-indexed, the request's Docling page_range)
// limits the pages rendered, so a document trimmed to a span gains no entries
// outside it. `stop` is polled before each page (a cancelled request, a
// passed deadline); once it answers true the pages rendered so far stay and
// the rest are skipped. The source's backend calls also run no later than
// `deadline`, and a render in flight is cancelled once `stop` answers true:
// a helper thread polls `stop` while pages render, so it must be safe to
// call from another thread. `passwords` are the request's candidates for an
// encrypted document: the collector that folded it opened it with one, and
// the render needs the same one.
[[nodiscard]] std::vector<Warning> attach_page_previews(
    std::shared_ptr<const std::string> bytes, ai::pipestream::document::v1::Document* document,
    std::optional<std::pair<int, int>> page_range = std::nullopt,
    const std::function<bool()>& stop = {},
    std::chrono::system_clock::time_point deadline = std::chrono::system_clock::time_point::max(),
    DocumentPasswords passwords = {});

}  // namespace grparse
