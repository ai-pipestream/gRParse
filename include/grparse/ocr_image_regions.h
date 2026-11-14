#pragma once

#include <vector>

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"

namespace grparse {

// The share of the page an image placement must cover to be read even when
// no layout region overlaps it (a full-width raster figure the detector
// split into icons, or missed).
inline constexpr double kImageReadMinPageShare = 0.01;

// The rectangles a page settled by its text layer is OCRed in, in raster
// pixels, clipped to the raster.
//
// Docling's default OCR mode reads every layout cluster that overlaps a
// bitmap, whatever its label; it keys on the PDF's own image placements,
// not on the picture label. Here a rectangle is:
//   - every picture region (as before: a chart's labels drawn into it), and
//   - every image placement that a layout region of any label overlaps, or
//     that covers at least `min_page_share` of the page on its own.
// The placement itself is read, not its intersection with the region: a
// detector that splits a raster figure into icons would otherwise read only
// the icons. Rectangles that overlap by more than half of the smaller one
// merge into their union, so a figure is read once. Placements narrower or
// shorter than `min_side` pixels are skipped. Pure.
std::vector<cv::Rect> ocr_image_rects(const std::vector<LayoutRegion>& regions,
                                      const std::vector<cv::Rect>& placements,
                                      int raster_width, int raster_height, int min_side,
                                      double min_page_share = kImageReadMinPageShare);

}  // namespace grparse
