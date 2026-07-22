#pragma once

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"

namespace grparse {

// The layout region a text line belongs to: the highest-confidence region
// whose box contains the line's box center, or nullptr when none does.  This
// single rule is shared by reading order, label mapping, and table structure
// so a line can never bind to different regions in different stages.
const LayoutRegion* region_for_line(const OcrPage& page, const OcrLine& line);

// Region box clipped to a raster of the given size.  Empty (zero-area) when
// the region lies entirely outside the raster or is degenerate.
cv::Rect clip_region(const LayoutRegion& region, int raster_width, int raster_height);

// Zero-copy view of the region's pixels, clipped to the raster.  The view
// aliases `raster` and must not outlive it; callers that need the crop past
// the raster's release point (the end of the inference stage) must clone.
// Returns an empty Mat when the clipped region has no area.
cv::Mat crop_region(const cv::Mat& raster, const LayoutRegion& region);

}  // namespace grparse
