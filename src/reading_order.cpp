#include "grparse/reading_order.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

#include "grparse/region_geometry.h"
#include "grparse/text_geometry.h"

namespace grparse {
namespace {

struct Gap {
  double width = 0;
  // Coordinate where the whitespace begins; boxes starting past it fall on
  // the far side of the split.
  double at = 0;
};

// Widest whitespace gap that no box crosses, along one axis.
std::optional<Gap> widest_gap(const std::vector<OrderBox>& boxes, const std::vector<size_t>& members,
                              bool horizontal) {
  std::vector<std::pair<double, double>> spans;
  spans.reserve(members.size());
  for (const size_t index : members) {
    const OrderBox& box = boxes[index];
    spans.emplace_back(horizontal ? box.top : box.left, horizontal ? box.bottom : box.right);
  }
  std::ranges::sort(spans);
  std::optional<Gap> best;
  double band_end = spans.front().second;
  for (const auto& [start, finish] : spans) {
    if (start > band_end && (!best || start - band_end > best->width)) {
      best = Gap{start - band_end, band_end};
    }
    band_end = std::max(band_end, finish);
  }
  return best;
}

// Whether the boxes on each side of a vertical gap run along enough of the
// block's height for the gap to be a column gutter rather than the space
// beside a short label.
bool gutter_has_two_sides(const std::vector<OrderBox>& boxes, const std::vector<size_t>& members,
                          const Gap& gap, double side_share) {
  if (side_share <= 0) return true;
  struct Extent {
    double top = std::numeric_limits<double>::infinity();
    double bottom = -std::numeric_limits<double>::infinity();
    double height() const { return bottom - top; }
  };
  Extent whole;
  Extent before;
  Extent after;
  for (const size_t index : members) {
    const OrderBox& box = boxes[index];
    Extent& side = box.left <= gap.at ? before : after;
    for (Extent* extent : {&whole, &side}) {
      extent->top = std::min(extent->top, box.top);
      extent->bottom = std::max(extent->bottom, box.bottom);
    }
  }
  if (whole.height() <= 0) return true;
  return before.height() >= side_share * whole.height() &&
         after.height() >= side_share * whole.height();
}

// Recursive cut at the widest whitespace gap on either axis, the policy
// arbitrating between the two.  Choosing the widest gap (not the first axis
// that has any gap) is what keeps line spacing inside a column from
// splitting rows before the column gutter is honoured; ties prefer the
// horizontal cut so bands read top to bottom.
void order_members(const std::vector<OrderBox>& boxes, const std::vector<size_t>& members,
                   const CutPolicy& policy, std::vector<size_t>* ordered) {
  if (members.size() <= 1) {
    ordered->insert(ordered->end(), members.begin(), members.end());
    return;
  }
  const auto y_gap = widest_gap(boxes, members, true);
  auto x_gap = widest_gap(boxes, members, false);
  if (x_gap && !gutter_has_two_sides(boxes, members, *x_gap, policy.gutter_side_share)) {
    x_gap.reset();
  }
  const bool cut_horizontal =
      y_gap && (!x_gap || y_gap->width >= policy.band_over_gutter * x_gap->width);
  const auto& gap = cut_horizontal ? y_gap : x_gap;
  if (gap) {
    std::vector<size_t> before;
    std::vector<size_t> after;
    for (const size_t index : members) {
      const double start = cut_horizontal ? boxes[index].top : boxes[index].left;
      (start <= gap->at ? before : after).push_back(index);
    }
    order_members(boxes, before, policy, ordered);
    order_members(boxes, after, policy, ordered);
    return;
  }
  // No whitespace separates anything: stable geometric order.
  std::vector<size_t> sorted = members;
  std::ranges::stable_sort(sorted, [&boxes](size_t a, size_t b) {
    if (boxes[a].top != boxes[b].top) return boxes[a].top < boxes[b].top;
    return boxes[a].left < boxes[b].left;
  });
  ordered->insert(ordered->end(), sorted.begin(), sorted.end());
}

struct Unit {
  AxisAlignedBox box;
  std::vector<size_t> line_indices;  // indices into page.lines, unsorted
};

}  // namespace

std::vector<size_t> xy_cut_order(const std::vector<OrderBox>& boxes, const CutPolicy& policy) {
  std::vector<size_t> members(boxes.size());
  for (size_t index = 0; index < boxes.size(); ++index) members[index] = index;
  std::vector<size_t> ordered;
  ordered.reserve(boxes.size());
  order_members(boxes, members, policy, &ordered);
  return ordered;
}

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
    const LayoutRegion* best = region_for_line(page, line);
    if (best != nullptr) {
      const auto region_index = static_cast<size_t>(best - page.regions.data());
      Unit& unit = units[static_cast<size_t>(region_unit[region_index])];
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
  std::vector<OrderBox> boxes;
  with_lines.reserve(units.size());
  boxes.reserve(units.size());
  for (const auto& unit : units) {
    if (unit.line_indices.empty()) continue;
    with_lines.push_back(&unit);
    boxes.push_back(OrderBox{static_cast<double>(unit.box.left), static_cast<double>(unit.box.top),
                             static_cast<double>(unit.box.right),
                             static_cast<double>(unit.box.bottom)});
  }

  std::vector<size_t> result;
  result.reserve(page.lines.size());
  for (const size_t unit_index : xy_cut_order(boxes)) {
    std::vector<size_t> lines = with_lines[unit_index]->line_indices;
    std::ranges::stable_sort(lines, [&page](size_t a, size_t b) {
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
