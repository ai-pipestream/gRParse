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

}  // namespace grparse
