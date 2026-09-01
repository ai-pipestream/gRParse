#pragma once

#include <algorithm>
#include <cstdint>

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"

namespace grparse {

struct AxisAlignedBox {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  // Widths and areas are computed in 64 bits: OCR polygons are model output and
  // a degenerate box must not silently wrap an int multiply.
  int64_t width() const { return static_cast<int64_t>(right) - left; }
  int64_t height() const { return static_cast<int64_t>(bottom) - top; }
  int64_t area() const { return std::max<int64_t>(0, width()) * std::max<int64_t>(0, height()); }
  cv::Point center() const {
    return {static_cast<int>((static_cast<int64_t>(left) + right) / 2),
            static_cast<int>((static_cast<int64_t>(top) + bottom) / 2)};
  }
  bool contains(cv::Point point) const {
    return point.x >= left && point.x <= right && point.y >= top && point.y <= bottom;
  }
};

AxisAlignedBox bounding_box(const OcrLine& line);
float intersection_over_union(const AxisAlignedBox& a, const AxisAlignedBox& b);
bool boxes_overlap_significantly(const AxisAlignedBox& a, const AxisAlignedBox& b,
                                 float iou_threshold = 0.25F);

// A text-direction vote over a page's line boxes. Text lines are wide and
// short; on a page rasterized a quarter turn from upright the detector
// still finds the lines, but as tall narrow boxes. The vote counts both
// shapes (a box is vertical when its height exceeds `aspect` times its
// width, horizontal when its width exceeds `aspect` times its height) and
// calls the page a quarter turn when at least `minimum_lines` lines were
// seen and vertical ones outnumber horizontal ones two to one. It cannot
// tell 90 from 270: the caller rotates the raster one way, re-recognizes,
// and keeps whichever orientation reads with the higher confidence.
struct RotationVote {
  int vertical_lines = 0;
  int horizontal_lines = 0;
  bool quarter_turn = false;
};

RotationVote page_rotation_vote(const OcrPage& page, double aspect = 1.5, int minimum_lines = 3);

}  // namespace grparse
