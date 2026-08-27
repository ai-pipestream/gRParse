#include "grparse/page_previews.h"

#include <algorithm>
#include <exception>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "grparse/document_assembly.h"
#include "grparse/in_memory_document.h"

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

void attach_page_previews(std::shared_ptr<const std::string> bytes,
                          ai::pipestream::document::v1::Document* document) {
  if (document == nullptr || bytes == nullptr) return;
  std::shared_ptr<PageSource> source;
  try {
    source = open_in_memory_document(std::move(bytes), /*pdf=*/true, /*pdf_parser_slots=*/1,
                                     kPreviewRenderDpi);
  } catch (const std::exception&) {
    return;
  }
  if (!source) return;
  const int pages = source->page_count();
  for (int page_no = 1; page_no <= pages; ++page_no) {
    cv::Mat raster;
    try {
      raster = source->render_page(page_no);
    } catch (const std::exception&) {
      continue;
    }
    if (raster.empty()) continue;
    std::vector<unsigned char> png;
    if (!cv::imencode(".png", preview_of(raster), png)) continue;
    auto& page = (*document->mutable_pages())[page_no];
    page.set_page_no(page_no);
    set_picture_image(png, page.mutable_image());
  }
}

}  // namespace grparse
