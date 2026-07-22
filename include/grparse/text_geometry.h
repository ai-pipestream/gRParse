#pragma once

#include <algorithm>

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"

namespace grparse {

struct AxisAlignedBox {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  int width() const { return right - left; }
  int height() const { return bottom - top; }
  int area() const { return std::max(0, width()) * std::max(0, height()); }
  cv::Point center() const { return {(left + right) / 2, (top + bottom) / 2}; }
  bool contains(cv::Point point) const {
    return point.x >= left && point.x <= right && point.y >= top && point.y <= bottom;
  }
};

AxisAlignedBox bounding_box(const OcrLine& line);
float intersection_over_union(const AxisAlignedBox& a, const AxisAlignedBox& b);
bool boxes_overlap_significantly(const AxisAlignedBox& a, const AxisAlignedBox& b,
                                 float iou_threshold = 0.25F);

}  // namespace grparse
