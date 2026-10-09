#pragma once

#include <vector>

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"

namespace grparse {

// The layout region a text line belongs to: the highest-confidence region
// whose box contains the line's box center, or nullptr when none does.  This
// single rule is shared by reading order, label mapping, and table structure
// so a line can never bind to different regions in different stages.
const LayoutRegion* region_for_line(const OcrPage& page, const OcrLine& line);

// Settles a page's raw detections before anything reads them, the subset of
// the reference layout postprocessor that decides which region owns a line
// (applied in this order):
//
// 1. A picture covering more than 90% of the page is the page itself, not a
//    figure on it (the reference drops such pictures), and is dropped when
//    some line's center lies inside it, so the page's text reads as its own
//    prose. A page without text keeps it: a photo scan has nothing else.
// 2. Tables that overlap (IoU, or either box's share inside the other, above
//    0.8) are one table detected twice, and each group keeps one: in effect
//    the largest, unless a member at least half its size scores more than
//    0.2 above it (region_geometry.cpp spells out the tie rules).
// 3. A region other than a table, picture, form, key-value region or
//    document index that lies more than 80% inside a kept table is part of
//    that table (the reference nests it under the table), and is dropped so
//    its lines bind to the table and land in its cells.
//
// `lines` are the page's lines in the regions' coordinate space; the page
// size is the raster the regions were detected on. Region order is kept.
void resolve_region_overlaps(std::vector<LayoutRegion>& regions, const std::vector<OcrLine>& lines,
                             int page_width, int page_height);

// Region box clipped to a raster of the given size.  Empty (zero-area) when
// the region lies entirely outside the raster or is degenerate.
cv::Rect clip_region(const LayoutRegion& region, int raster_width, int raster_height);

// Zero-copy view of the region's pixels, clipped to the raster.  The view
// aliases `raster` and must not outlive it; callers that need the crop past
// the raster's release point (the end of the inference stage) must clone.
// Returns an empty Mat when the clipped region has no area.
cv::Mat crop_region(const cv::Mat& raster, const LayoutRegion& region);

}  // namespace grparse
