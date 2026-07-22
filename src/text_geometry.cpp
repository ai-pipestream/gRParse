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
  const int64_t left = std::max<int64_t>(a.left, b.left);
  const int64_t top = std::max<int64_t>(a.top, b.top);
  const int64_t right = std::min<int64_t>(a.right, b.right);
  const int64_t bottom = std::min<int64_t>(a.bottom, b.bottom);
  const int64_t intersection = std::max<int64_t>(0, right - left) * std::max<int64_t>(0, bottom - top);
  if (intersection <= 0) return 0.0F;
  const int64_t union_area = a.area() + b.area() - intersection;
  if (union_area <= 0) return 0.0F;
  return static_cast<float>(static_cast<double>(intersection) / static_cast<double>(union_area));
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

  // Bounding boxes are computed once per line and carried through dedup and
  // sorting.  Recomputing them inside the comparator rescanned every polygon
  // O(n log n) times on pages that can carry thousands of lines.
  struct Entry {
    OcrLine line;
    AxisAlignedBox box;
  };
  std::vector<Entry> entries;
  entries.reserve(digital.lines.size() + ocr.lines.size());

  for (auto& line : digital.lines) {
    if (line.text.empty() || line.polygon.empty()) continue;
    const AxisAlignedBox box = bounding_box(line);
    if (!line.origin.has_value()) line.origin = TextOrigin::kDigitalPdf;
    entries.push_back(Entry{std::move(line), box});
  }
  const size_t digital_count = entries.size();

  for (auto& line : ocr.lines) {
    if (line.text.empty() || line.polygon.empty()) continue;
    const AxisAlignedBox box = bounding_box(line);
    bool duplicate = false;
    for (size_t index = 0; index < digital_count; ++index) {
      if (boxes_overlap_significantly(entries[index].box, box)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    if (!line.origin.has_value()) line.origin = TextOrigin::kOcr;
    entries.push_back(Entry{std::move(line), box});
  }

  std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.box.top != b.box.top) return a.box.top < b.box.top;
    return a.box.left < b.box.left;
  });
  merged.lines.reserve(entries.size());
  for (auto& entry : entries) merged.lines.push_back(std::move(entry.line));
  return merged;
}

}  // namespace grparse
