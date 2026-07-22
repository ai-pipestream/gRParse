#include "grparse/reading_order.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "grparse/text_geometry.h"

namespace grparse {
namespace {

struct Unit {
  AxisAlignedBox box;
  std::vector<size_t> line_indices;  // indices into page.lines, unsorted
};

struct Gap {
  int64_t width = 0;
  // Coordinate where the whitespace begins; units starting past it fall on
  // the far side of the split.
  int64_t at = 0;
};

// Widest whitespace gap that no unit crosses, along one axis.
std::optional<Gap> widest_gap(const std::vector<const Unit*>& units, bool horizontal) {
  std::vector<std::pair<int64_t, int64_t>> spans;
  spans.reserve(units.size());
  for (const Unit* unit : units) {
    spans.emplace_back(horizontal ? unit->box.top : unit->box.left,
                       horizontal ? unit->box.bottom : unit->box.right);
  }
  std::sort(spans.begin(), spans.end());
  std::optional<Gap> best;
  int64_t band_end = spans.front().second;
  for (const auto& [start, finish] : spans) {
    if (start > band_end && (!best || start - band_end > best->width)) {
      best = Gap{start - band_end, band_end};
    }
    band_end = std::max(band_end, finish);
  }
  return best;
}

// Recursive cut at the single widest whitespace gap on either axis.  Choosing
// the widest gap (not the first axis that has any gap) is what keeps line
// spacing inside a column from splitting rows before the column gutter is
// honoured; ties prefer the horizontal cut so bands read top to bottom.
void order_units(const std::vector<const Unit*>& units, std::vector<const Unit*>* ordered) {
  if (units.size() <= 1) {
    ordered->insert(ordered->end(), units.begin(), units.end());
    return;
  }
  const auto y_gap = widest_gap(units, true);
  const auto x_gap = widest_gap(units, false);
  const bool cut_horizontal = y_gap && (!x_gap || y_gap->width >= x_gap->width);
  const auto& gap = cut_horizontal ? y_gap : x_gap;
  if (gap) {
    std::vector<const Unit*> before;
    std::vector<const Unit*> after;
    for (const Unit* unit : units) {
      const int64_t start = cut_horizontal ? unit->box.top : unit->box.left;
      (start <= gap->at ? before : after).push_back(unit);
    }
    order_units(before, ordered);
    order_units(after, ordered);
    return;
  }
  // No whitespace separates anything: stable geometric order.
  std::vector<const Unit*> sorted = units;
  std::stable_sort(sorted.begin(), sorted.end(), [](const Unit* a, const Unit* b) {
    if (a->box.top != b->box.top) return a->box.top < b->box.top;
    return a->box.left < b->box.left;
  });
  ordered->insert(ordered->end(), sorted.begin(), sorted.end());
}

}  // namespace

std::vector<size_t> reading_order(const OcrPage& page) {
  std::vector<Unit> units;
  units.reserve(page.regions.size() + page.lines.size());

  // One unit per region; region boxes start from the detection and grow to
  // cover their member lines so slight under-detection cannot re-split text.
  std::vector<int> region_unit(page.regions.size(), -1);
  for (size_t index = 0; index < page.regions.size(); ++index) {
    const auto& region = page.regions[index];
    region_unit[index] = static_cast<int>(units.size());
    units.push_back(Unit{AxisAlignedBox{region.left, region.top, region.right, region.bottom}, {}});
  }

  for (size_t line_index = 0; line_index < page.lines.size(); ++line_index) {
    const auto& line = page.lines[line_index];
    if (line.text.empty() || line.polygon.empty()) continue;
    const AxisAlignedBox box = bounding_box(line);
    const cv::Point center = box.center();
    int best = -1;
    float best_confidence = 0.0F;
    for (size_t region_index = 0; region_index < page.regions.size(); ++region_index) {
      const auto& region = page.regions[region_index];
      const bool contains = center.x >= region.left && center.x <= region.right &&
                            center.y >= region.top && center.y <= region.bottom;
      if (contains && (best < 0 || region.confidence > best_confidence)) {
        best = static_cast<int>(region_index);
        best_confidence = region.confidence;
      }
    }
    if (best >= 0) {
      Unit& unit = units[static_cast<size_t>(region_unit[static_cast<size_t>(best)])];
      unit.line_indices.push_back(line_index);
      unit.box.left = std::min(unit.box.left, box.left);
      unit.box.top = std::min(unit.box.top, box.top);
      unit.box.right = std::max(unit.box.right, box.right);
      unit.box.bottom = std::max(unit.box.bottom, box.bottom);
    } else {
      units.push_back(Unit{box, {line_index}});
    }
  }

  // Regions with no text (figures, empty tables) carry no lines and drop out
  // of the text order naturally.
  std::vector<const Unit*> with_lines;
  with_lines.reserve(units.size());
  for (const auto& unit : units) {
    if (!unit.line_indices.empty()) with_lines.push_back(&unit);
  }

  std::vector<const Unit*> ordered;
  ordered.reserve(with_lines.size());
  order_units(with_lines, &ordered);

  std::vector<size_t> result;
  result.reserve(page.lines.size());
  for (const Unit* unit : ordered) {
    std::vector<size_t> lines = unit->line_indices;
    std::stable_sort(lines.begin(), lines.end(), [&page](size_t a, size_t b) {
      const AxisAlignedBox box_a = bounding_box(page.lines[a]);
      const AxisAlignedBox box_b = bounding_box(page.lines[b]);
      if (box_a.top != box_b.top) return box_a.top < box_b.top;
      return box_a.left < box_b.left;
    });
    result.insert(result.end(), lines.begin(), lines.end());
  }
  return result;
}

}  // namespace grparse
