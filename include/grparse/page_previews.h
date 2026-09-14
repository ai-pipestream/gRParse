#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

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
void attach_page_previews(std::shared_ptr<const std::string> bytes,
                          ai::pipestream::document::v1::Document* document);

}  // namespace grparse
