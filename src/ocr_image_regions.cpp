#include "grparse/ocr_image_regions.h"

#include <algorithm>
#include <cstdint>

namespace grparse {

namespace {

cv::Rect clipped(int left, int top, int right, int bottom, int width, int height) {
  const int x0 = std::clamp(left, 0, width);
  const int y0 = std::clamp(top, 0, height);
  const int x1 = std::clamp(right, 0, width);
  const int y1 = std::clamp(bottom, 0, height);
  return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

bool overlaps(const cv::Rect& a, const cv::Rect& b) { return (a & b).area() > 0; }

// More than half of the smaller rectangle lies inside the other.
bool mostly_shared(const cv::Rect& a, const cv::Rect& b) {
  const int64_t shared = (a & b).area();
  const int64_t smaller = std::min<int64_t>(a.area(), b.area());
  return smaller > 0 && 2 * shared > smaller;
}

}  // namespace

std::vector<cv::Rect> ocr_image_rects(const std::vector<LayoutRegion>& regions,
                                      const std::vector<cv::Rect>& placements,
                                      int raster_width, int raster_height, int min_side,
                                      double min_page_share) {
  std::vector<cv::Rect> pictures;
  std::vector<cv::Rect> region_rects;
  for (const auto& region : regions) {
    const cv::Rect rect = clipped(region.left, region.top, region.right, region.bottom, raster_width,
                                  raster_height);
    if (rect.area() == 0) continue;
    region_rects.push_back(rect);
    if (region.label == "picture" && rect.width >= min_side && rect.height >= min_side) {
      pictures.push_back(rect);
    }
  }
  // Placements the picture regions do not already read: a strip of chart
  // panels is read panel by panel as before, since one wide read of the
  // whole strip can turn it upside down and lose the small labels.
  std::vector<cv::Rect> extra;
  const double page_area = static_cast<double>(raster_width) * raster_height;
  for (const auto& placement : placements) {
    const cv::Rect rect = clipped(placement.x, placement.y, placement.x + placement.width,
                                  placement.y + placement.height, raster_width, raster_height);
    if (rect.width < min_side || rect.height < min_side) continue;
    const bool covered = std::ranges::any_of(region_rects, [&rect](const cv::Rect& r) { return overlaps(r, rect); });
    const bool large = page_area > 0 && rect.area() >= min_page_share * page_area;
    if (!covered && !large) continue;
    int64_t read = 0;
    for (const cv::Rect& picture : pictures) read += (picture & rect).area();
    if (2 * read >= rect.area()) continue;
    extra.push_back(rect);
  }
  // Placements that overlap by more than half of the smaller one are one read.
  for (bool merged = true; merged;) {
    merged = false;
    for (size_t i = 0; i < extra.size() && !merged; ++i) {
      for (size_t j = i + 1; j < extra.size(); ++j) {
        if (!mostly_shared(extra[i], extra[j])) continue;
        extra[i] |= extra[j];
        extra.erase(extra.begin() + static_cast<std::ptrdiff_t>(j));
        merged = true;
        break;
      }
    }
  }
  const auto top_down = [](const cv::Rect& a, const cv::Rect& b) {
    return a.y != b.y ? a.y < b.y : a.x < b.x;
  };
  std::ranges::sort(pictures, top_down);
  std::ranges::sort(extra, top_down);
  // Pictures first: their lines win where a placement read overlaps them.
  pictures.insert(pictures.end(), extra.begin(), extra.end());
  return pictures;
}

}  // namespace grparse
