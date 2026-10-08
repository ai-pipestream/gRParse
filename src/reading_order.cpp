#include "grparse/reading_order.h"

#include <algorithm>
#include <cmath>
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

using Rows = std::vector<std::pair<double, double>>;

// The rows one side of a vertical gap occupies: the vertical spans of its
// boxes, sorted and merged where they overlap.
Rows side_rows(const std::vector<OrderBox>& boxes, const std::vector<size_t>& members,
               const Gap& gap, bool after) {
  Rows spans;
  for (const size_t index : members) {
    const OrderBox& box = boxes[index];
    if ((box.left > gap.at) == after) spans.emplace_back(box.top, box.bottom);
  }
  std::ranges::sort(spans);
  Rows merged;
  for (const auto& [start, finish] : spans) {
    if (!merged.empty() && start <= merged.back().second) {
      merged.back().second = std::max(merged.back().second, finish);
    } else {
      merged.emplace_back(start, finish);
    }
  }
  return merged;
}

double total_length(const Rows& rows) {
  double total = 0;
  for (const auto& [start, finish] : rows) total += finish - start;
  return total;
}

// The length over which two sets of merged rows are level with each other.
double shared_length(const Rows& a, const Rows& b) {
  double total = 0;
  for (const auto& [a_start, a_finish] : a) {
    for (const auto& [b_start, b_finish] : b) {
      total += std::max(0.0, std::min(a_finish, b_finish) - std::max(a_start, b_start));
    }
  }
  return total;
}

// Whether the boxes on each side of a vertical gap run beside each other:
// the rows the two sides share, as a share of the side with less row
// length, reach the policy's threshold. Two columns read in parallel; a
// label whose value sits in the label column's own whitespace does not.
bool gutter_sides_run_parallel(const std::vector<OrderBox>& boxes, const std::vector<size_t>& members,
                               const Gap& gap, double share) {
  if (share <= 0) return true;
  const Rows before = side_rows(boxes, members, gap, false);
  const Rows after = side_rows(boxes, members, gap, true);
  const double shorter = std::min(total_length(before), total_length(after));
  if (shorter <= 0) return true;
  return shared_length(before, after) >= share * shorter;
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
  if (x_gap && (!gutter_has_two_sides(boxes, members, *x_gap, policy.gutter_side_share) ||
                !gutter_sides_run_parallel(boxes, members, *x_gap, policy.gutter_parallel_share))) {
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
  // A box with a non-finite edge never enters the sorts, whose comparators
  // need a strict weak ordering that NaN breaks; such boxes follow the rest
  // in input order.
  std::vector<size_t> members;
  std::vector<size_t> unplaced;
  members.reserve(boxes.size());
  for (size_t index = 0; index < boxes.size(); ++index) {
    const OrderBox& box = boxes[index];
    const bool finite = std::isfinite(box.left) && std::isfinite(box.top) &&
                        std::isfinite(box.right) && std::isfinite(box.bottom);
    (finite ? members : unplaced).push_back(index);
  }
  std::vector<size_t> ordered;
  ordered.reserve(boxes.size());
  order_members(boxes, members, policy, &ordered);
  ordered.insert(ordered.end(), unplaced.begin(), unplaced.end());
  return ordered;
}

std::vector<size_t> reading_order(const OcrPage& page, bool trust_source_order) {
  std::vector<Unit> units;
  units.reserve(page.regions.size() + page.lines.size());

  // One unit per region. The detection decides membership (region_for_line);
  // the unit's box for the cut is the hull of its member lines, because a
  // detector box drawn a line too tall or too wide would otherwise claim its
  // neighbour's row or cross a gutter, and the cut then reads the two out of
  // order. A region without lines keeps the detection as its box and drops
  // out of the order below anyway.
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
      if (unit.line_indices.empty()) {
        unit.box = box;
      } else {
        unit.box.left = std::min(unit.box.left, box.left);
        unit.box.top = std::min(unit.box.top, box.top);
        unit.box.right = std::max(unit.box.right, box.right);
        unit.box.bottom = std::max(unit.box.bottom, box.bottom);
      }
      unit.line_indices.push_back(line_index);
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
  if (trust_source_order) {
    // The source's emission order is the reading order: units order by
    // their first line's emission index, lines within a unit by emission
    // index. The unit construction above (region binding included) is the
    // same as the geometric path; only the sort key changes, so the result
    // stays deterministic in the page's emission order.
    std::vector<size_t> unit_order(with_lines.size());
    for (size_t index = 0; index < with_lines.size(); ++index) unit_order[index] = index;
    std::ranges::stable_sort(unit_order, [&with_lines](size_t a, size_t b) {
      return with_lines[a]->line_indices.front() < with_lines[b]->line_indices.front();
    });
    for (const size_t unit_index : unit_order) {
      std::vector<size_t> lines = with_lines[unit_index]->line_indices;
      std::ranges::stable_sort(lines);
      result.insert(result.end(), lines.begin(), lines.end());
    }
    return result;
  }
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
