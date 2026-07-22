#include "grparse/table_structure.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>

#include "grparse/region_geometry.h"

namespace grparse {
namespace {

struct Entry {
  size_t line_index = 0;
  AxisAlignedBox box;
};

// Row bands by vertical clustering: entries are walked top-to-bottom and an
// entry joins the current band when its vertical center falls inside it.
// OCR boxes in adjacent table rows are separated by the row leading, so the
// center test keeps tight rows apart while tolerating ragged box edges.
std::vector<std::vector<Entry>> cluster_rows(std::vector<Entry> entries) {
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.box.top != b.box.top) return a.box.top < b.box.top;
    return a.box.left < b.box.left;
  });
  std::vector<std::vector<Entry>> bands;
  int64_t band_bottom = std::numeric_limits<int64_t>::min();
  for (Entry& entry : entries) {
    const int64_t center = (static_cast<int64_t>(entry.box.top) + entry.box.bottom) / 2;
    if (bands.empty() || center > band_bottom) {
      bands.emplace_back();
      band_bottom = entry.box.bottom;
    } else {
      band_bottom = std::max(band_bottom, static_cast<int64_t>(entry.box.bottom));
    }
    bands.back().push_back(std::move(entry));
  }
  return bands;
}

// Column spans by merging the horizontal extents of every entry.  Gaps
// narrower than the gutter threshold (about half a line height, roughly one
// character) are word spacing inside a cell; wider gaps separate columns.
std::vector<std::pair<int, int>> column_spans(const std::vector<Entry>& entries,
                                              int gutter_threshold) {
  std::vector<std::pair<int, int>> spans;
  spans.reserve(entries.size());
  for (const Entry& entry : entries) spans.emplace_back(entry.box.left, entry.box.right);
  std::sort(spans.begin(), spans.end());
  std::vector<std::pair<int, int>> merged;
  for (const auto& [left, right] : spans) {
    if (!merged.empty() && left <= merged.back().second + gutter_threshold) {
      merged.back().second = std::max(merged.back().second, right);
    } else {
      merged.emplace_back(left, right);
    }
  }
  return merged;
}

// Column with the largest horizontal overlap; nearest center on no overlap.
size_t column_for(const AxisAlignedBox& box, const std::vector<std::pair<int, int>>& columns) {
  size_t best = 0;
  int64_t best_overlap = std::numeric_limits<int64_t>::min();
  const int64_t center = (static_cast<int64_t>(box.left) + box.right) / 2;
  for (size_t index = 0; index < columns.size(); ++index) {
    const auto& [left, right] = columns[index];
    int64_t overlap = std::min<int64_t>(box.right, right) - std::max<int64_t>(box.left, left);
    if (overlap <= 0) {
      // Negative distance so any true overlap beats every miss.
      overlap = -std::abs(center - (static_cast<int64_t>(left) + right) / 2);
    }
    if (overlap > best_overlap) {
      best_overlap = overlap;
      best = index;
    }
  }
  return best;
}

int median_height(const std::vector<Entry>& entries) {
  std::vector<int64_t> heights;
  heights.reserve(entries.size());
  for (const Entry& entry : entries) heights.push_back(entry.box.height());
  std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
  return static_cast<int>(heights[heights.size() / 2]);
}

}  // namespace

TableGrid build_table_grid(const OcrPage& page, const LayoutRegion& table) {
  std::vector<Entry> entries;
  for (size_t index = 0; index < page.lines.size(); ++index) {
    const auto& line = page.lines[index];
    if (line.text.empty() || line.polygon.empty()) continue;
    if (region_for_line(page, line) == &table) entries.push_back({index, bounding_box(line)});
  }
  if (entries.empty()) return {};

  const int gutter = std::max(2, median_height(entries) / 2);
  const auto columns = column_spans(entries, gutter);
  const auto bands = cluster_rows(std::move(entries));

  TableGrid grid;
  grid.rows = static_cast<int>(bands.size());
  grid.cols = static_cast<int>(columns.size());
  grid.cells.resize(static_cast<size_t>(grid.rows) * grid.cols);
  for (int row = 0; row < grid.rows; ++row) {
    for (int col = 0; col < grid.cols; ++col) {
      auto& cell = grid.cells[static_cast<size_t>(row) * grid.cols + col];
      cell.row = row;
      cell.col = col;
    }
  }
  for (size_t row = 0; row < bands.size(); ++row) {
    for (const Entry& entry : bands[row]) {
      const size_t col = column_for(entry.box, columns);
      auto& cell = grid.cells[row * columns.size() + col];
      if (cell.line_indices.empty()) {
        cell.box = entry.box;
      } else {
        cell.box.left = std::min(cell.box.left, entry.box.left);
        cell.box.top = std::min(cell.box.top, entry.box.top);
        cell.box.right = std::max(cell.box.right, entry.box.right);
        cell.box.bottom = std::max(cell.box.bottom, entry.box.bottom);
      }
      cell.line_indices.push_back(entry.line_index);
    }
  }
  // Band iteration is (top, left) sorted, so line_indices are already in
  // reading order within each cell.
  return grid;
}

}  // namespace grparse
