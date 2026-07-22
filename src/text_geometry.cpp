#include "grparse/text_geometry.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace grparse {

AxisAlignedBox bounding_box(const OcrLine& line) {
  AxisAlignedBox box{std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
                     std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
  for (const auto& point : line.polygon) {
    box.left = std::min(box.left, point.x);
    box.top = std::min(box.top, point.y);
    box.right = std::max(box.right, point.x);
    box.bottom = std::max(box.bottom, point.y);
  }
  if (line.polygon.empty()) {
    box = {};
  }
  return box;
}

float intersection_over_union(const AxisAlignedBox& a, const AxisAlignedBox& b) {
  const int left = std::max(a.left, b.left);
  const int top = std::max(a.top, b.top);
  const int right = std::min(a.right, b.right);
  const int bottom = std::min(a.bottom, b.bottom);
  const int intersection = std::max(0, right - left) * std::max(0, bottom - top);
  if (intersection == 0) return 0.0F;
  const int union_area = a.area() + b.area() - intersection;
  if (union_area <= 0) return 0.0F;
  return static_cast<float>(intersection) / static_cast<float>(union_area);
}

bool boxes_overlap_significantly(const AxisAlignedBox& a, const AxisAlignedBox& b,
                                 float iou_threshold) {
  if (a.contains(b.center()) || b.contains(a.center())) return true;
  return intersection_over_union(a, b) >= iou_threshold;
}

OcrPage merge_digital_and_ocr(OcrPage digital, OcrPage ocr) {
  OcrPage merged;
  merged.width = digital.width > 0 ? digital.width : ocr.width;
  merged.height = digital.height > 0 ? digital.height : ocr.height;
  merged.source = OcrPage::Source::kMerged;
  merged.skip_ocr = false;
  merged.lines.reserve(digital.lines.size() + ocr.lines.size());

  std::vector<AxisAlignedBox> digital_boxes;
  digital_boxes.reserve(digital.lines.size());
  for (auto& line : digital.lines) {
    if (line.text.empty() || line.polygon.empty()) continue;
    if (!line.origin.has_value()) line.origin = TextOrigin::kDigitalPdf;
    digital_boxes.push_back(bounding_box(line));
    merged.lines.push_back(std::move(line));
  }

  for (auto& line : ocr.lines) {
    if (line.text.empty() || line.polygon.empty()) continue;
    const AxisAlignedBox box = bounding_box(line);
    bool duplicate = false;
    for (const auto& digital_box : digital_boxes) {
      if (boxes_overlap_significantly(digital_box, box)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    if (!line.origin.has_value()) line.origin = TextOrigin::kOcr;
    merged.lines.push_back(std::move(line));
  }

  std::stable_sort(merged.lines.begin(), merged.lines.end(), [](const OcrLine& a, const OcrLine& b) {
    const auto ba = bounding_box(a);
    const auto bb = bounding_box(b);
    if (ba.top != bb.top) return ba.top < bb.top;
    return ba.left < bb.left;
  });
  return merged;
}

}  // namespace grparse
