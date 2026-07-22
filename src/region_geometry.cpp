#include "grparse/region_geometry.h"

#include <algorithm>

#include "grparse/text_geometry.h"

namespace grparse {

const LayoutRegion* region_for_line(const OcrPage& page, const OcrLine& line) {
  if (page.regions.empty() || line.polygon.empty()) return nullptr;
  const cv::Point center = bounding_box(line).center();
  const LayoutRegion* best = nullptr;
  for (const auto& region : page.regions) {
    const bool contains = center.x >= region.left && center.x <= region.right &&
                          center.y >= region.top && center.y <= region.bottom;
    if (contains && (best == nullptr || region.confidence > best->confidence)) {
      best = &region;
    }
  }
  return best;
}

cv::Rect clip_region(const LayoutRegion& region, int raster_width, int raster_height) {
  const int left = std::clamp(region.left, 0, raster_width);
  const int top = std::clamp(region.top, 0, raster_height);
  const int right = std::clamp(region.right, 0, raster_width);
  const int bottom = std::clamp(region.bottom, 0, raster_height);
  if (right <= left || bottom <= top) return {};
  return {left, top, right - left, bottom - top};
}

cv::Mat crop_region(const cv::Mat& raster, const LayoutRegion& region) {
  const cv::Rect roi = clip_region(region, raster.cols, raster.rows);
  if (roi.empty()) return {};
  return raster(roi);
}

}  // namespace grparse
