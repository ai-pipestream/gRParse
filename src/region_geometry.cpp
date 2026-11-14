#include "grparse/region_geometry.h"

#include <algorithm>
#include <numeric>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

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
  if (best == nullptr || best->label != "picture") return best;
  // Inside a picture, a panel's own caption takes its line, and otherwise
  // the innermost picture does, so a panel's labels hang off the panel.
  const auto contains_center = [&center](const LayoutRegion& region) {
    return center.x >= region.left && center.x <= region.right && center.y >= region.top &&
           center.y <= region.bottom;
  };
  const auto box_area = [](const LayoutRegion& region) {
    return static_cast<double>(std::max(0, region.right - region.left)) *
           std::max(0, region.bottom - region.top);
  };
  const LayoutRegion* picture = best;
  for (const auto& region : page.regions) {
    if (&region == best || !contains_center(region) || !region_nested_in(region, *best)) continue;
    if (region.label == "caption") return &region;
    if (region.label == "picture" && box_area(region) < box_area(*picture)) picture = &region;
  }
  return picture;
}

namespace {

// The reference postprocessor's thresholds: a picture over kFullPageShare
// of the page is the page; two tables are one detection when their IoU, or
// either one's share inside the other, exceeds kSameDetection; a region
// with more than kNestedShare of its box inside a table belongs to it; a
// table group member loses to one scoring kTableConfidenceMargin higher
// unless it is over kTableAreaRatio times that one's size.
constexpr double kFullPageShare = 0.9;
constexpr double kSameDetection = 0.8;
constexpr double kNestedShare = 0.8;
constexpr double kTableConfidenceMargin = 0.2;
constexpr double kTableAreaRatio = 2.0;

double area(const LayoutRegion& region) {
  return std::max(0.0, static_cast<double>(region.right) - region.left) *
         std::max(0.0, static_cast<double>(region.bottom) - region.top);
}

double intersection(const LayoutRegion& a, const LayoutRegion& b) {
  const double width = std::min(a.right, b.right) - std::max(a.left, b.left);
  const double height = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
  return width > 0 && height > 0 ? width * height : 0.0;
}

// The share of `inner` that lies inside `outer`.
double inside_share(const LayoutRegion& inner, const LayoutRegion& outer) {
  const double own = area(inner);
  return own > 0 ? intersection(inner, outer) / own : 0.0;
}

bool same_detection(const LayoutRegion& a, const LayoutRegion& b) {
  const double shared = intersection(a, b);
  if (shared <= 0) return false;
  const double either = area(a) + area(b) - shared;
  return shared / either > kSameDetection || inside_share(a, b) > kSameDetection ||
         inside_share(b, a) > kSameDetection;
}

bool holds_a_line(const LayoutRegion& region, const std::vector<OcrLine>& lines) {
  return std::ranges::any_of(lines, [&region](const OcrLine& line) {
    if (line.text.empty() || line.polygon.empty()) return false;
    const cv::Point center = bounding_box(line).center();
    return center.x >= region.left && center.x <= region.right && center.y >= region.top &&
           center.y <= region.bottom;
  });
}

// Lines inside the region that open a figure or table caption. Two of them
// mean the region boxes several captioned figures, not one figure.
size_t captions_opened_in(const LayoutRegion& region, const std::vector<OcrLine>& lines) {
  return static_cast<size_t>(std::ranges::count_if(lines, [&region](const OcrLine& line) {
    if (line.polygon.empty()) return false;
    const std::string_view text = line.text;
    if (!text.starts_with("Fig") && !text.starts_with("FIG") && !text.starts_with("Table") &&
        !text.starts_with("TABLE")) {
      return false;
    }
    const cv::Point center = bounding_box(line).center();
    return center.x >= region.left && center.x <= region.right && center.y >= region.top &&
           center.y <= region.bottom;
  }));
}

// Labels the reference never nests inside a table: the floats and wrappers.
bool nests_in_tables(std::string_view label) {
  return label != "table" && label != "picture" && label != "form" &&
         label != "key_value_region" && label != "document_index";
}

// The table one group of overlapping tables keeps, by the reference's rule:
// a candidate is out of the running when another member scores more than
// the margin above it while it is at most the area ratio times that one's
// size; of the rest, a larger one replaces the pick unless the pick scores
// more than the margin above it. Members are in region order.
size_t kept_table(const std::vector<LayoutRegion>& regions, const std::vector<size_t>& group) {
  std::optional<size_t> best;
  for (const size_t candidate : group) {
    const bool eligible = std::ranges::none_of(group, [&](size_t other) {
      if (other == candidate || area(regions[other]) <= 0) return false;
      return area(regions[candidate]) / area(regions[other]) <= kTableAreaRatio &&
             regions[other].confidence - regions[candidate].confidence > kTableConfidenceMargin;
    });
    if (!eligible) continue;
    if (!best || (area(regions[candidate]) > area(regions[*best]) &&
                  regions[*best].confidence - regions[candidate].confidence <=
                      kTableConfidenceMargin)) {
      best = candidate;
    }
  }
  return best.value_or(group.front());
}

}  // namespace

bool region_nested_in(const LayoutRegion& inner, const LayoutRegion& outer) {
  return inside_share(inner, outer) > kNestedShare;
}

void resolve_region_overlaps(std::vector<LayoutRegion>& regions, const std::vector<OcrLine>& lines,
                             int page_width, int page_height) {
  std::vector<bool> dropped(regions.size(), false);
  const double page_area = static_cast<double>(std::max(page_width, 0)) * std::max(page_height, 0);
  if (page_area > 0) {
    for (size_t index = 0; index < regions.size(); ++index) {
      const LayoutRegion& region = regions[index];
      if (region.label == "picture" && area(region) / page_area > kFullPageShare &&
          holds_a_line(region, lines)) {
        dropped[index] = true;
      }
    }
  }

  // Overlapping tables group transitively; each group keeps one.
  std::vector<size_t> tables;
  for (size_t index = 0; index < regions.size(); ++index) {
    if (regions[index].label == "table") tables.push_back(index);
  }
  std::vector<size_t> root(tables.size());
  std::iota(root.begin(), root.end(), size_t{0});
  const auto group_of = [&root](size_t at) {
    while (root[at] != at) at = root[at] = root[root[at]];
    return at;
  };
  for (size_t a = 0; a < tables.size(); ++a) {
    for (size_t b = a + 1; b < tables.size(); ++b) {
      if (same_detection(regions[tables[a]], regions[tables[b]])) root[group_of(b)] = group_of(a);
    }
  }
  for (size_t a = 0; a < tables.size(); ++a) {
    if (group_of(a) != a) continue;
    std::vector<size_t> group;
    for (size_t b = 0; b < tables.size(); ++b) {
      if (group_of(b) == a) group.push_back(tables[b]);
    }
    if (group.size() < 2) continue;
    const size_t keep = kept_table(regions, group);
    for (const size_t member : group) dropped[member] = member != keep;
  }

  for (size_t index = 0; index < regions.size(); ++index) {
    if (dropped[index] || !nests_in_tables(regions[index].label)) continue;
    for (const size_t table : tables) {
      if (!dropped[table] && inside_share(regions[index], regions[table]) > kNestedShare) {
        dropped[index] = true;
        break;
      }
    }
  }

  // Pictures: a box around two or more detected pictures is a figure made
  // of panels, (a) to (d) under one caption. Both stay: the assembler hangs
  // the panels off the figure. When that box holds two or more captions of
  // its own ("Figure 13", "Figure 14"), it is several figures boxed again as
  // one and goes. A picture inside another that holds no second picture is
  // the same figure detected twice, and the smaller goes.
  std::vector<size_t> pictures;
  for (size_t index = 0; index < regions.size(); ++index) {
    if (!dropped[index] && regions[index].label == "picture") pictures.push_back(index);
  }
  std::vector<bool> has_panels(regions.size(), false);
  for (const size_t outer : pictures) {
    size_t inside = 0;
    for (const size_t inner : pictures) {
      if (inner != outer && inside_share(regions[inner], regions[outer]) > kNestedShare) ++inside;
    }
    if (inside < 2) continue;
    if (captions_opened_in(regions[outer], lines) >= 2) {
      dropped[outer] = true;
    } else {
      has_panels[outer] = true;
    }
  }
  // A box around some of a figure's panels, inside the figure and with no
  // caption of its own, is a grouping the detector drew (a column of two
  // panels): it goes, and its panels hang off the figure itself.
  for (const size_t inner : pictures) {
    if (dropped[inner] || !has_panels[inner]) continue;
    for (const size_t outer : pictures) {
      if (outer == inner || dropped[outer] || !has_panels[outer]) continue;
      if (inside_share(regions[inner], regions[outer]) > kNestedShare &&
          area(regions[inner]) < area(regions[outer]) &&
          captions_opened_in(regions[inner], lines) == 0) {
        dropped[inner] = true;
        break;
      }
    }
  }
  for (const size_t inner : pictures) {
    if (dropped[inner]) continue;
    for (const size_t outer : pictures) {
      if (outer == inner || dropped[outer] || has_panels[outer]) continue;
      if (inside_share(regions[inner], regions[outer]) > kNestedShare &&
          area(regions[inner]) < area(regions[outer])) {
        dropped[inner] = true;
        break;
      }
    }
  }

  std::vector<LayoutRegion> kept;
  kept.reserve(regions.size());
  for (size_t index = 0; index < regions.size(); ++index) {
    if (!dropped[index]) kept.push_back(std::move(regions[index]));
  }
  regions = std::move(kept);
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
