#include "grparse/table_structure.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
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
  std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
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
  std::ranges::sort(spans);
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
  std::ranges::nth_element(heights, heights.begin() + heights.size() / 2);
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

namespace {

bool center_inside(const AxisAlignedBox& box, const StructuredCell& cell) {
  const cv::Point center = box.center();
  return center.x >= cell.left && center.x <= cell.right && center.y >= cell.top &&
         center.y <= cell.bottom;
}

int64_t overlap_area(const AxisAlignedBox& box, const StructuredCell& cell) {
  const int64_t width =
      static_cast<int64_t>(std::min(box.right, cell.right)) - std::max(box.left, cell.left);
  const int64_t height =
      static_cast<int64_t>(std::min(box.bottom, cell.bottom)) - std::max(box.top, cell.top);
  return width > 0 && height > 0 ? width * height : 0;
}

int64_t axis_gap(int low_a, int high_a, int low_b, int high_b) {
  return std::max<int64_t>({0, static_cast<int64_t>(low_b) - high_a, static_cast<int64_t>(low_a) - high_b});
}

// Rows of lines past a cut-off decode, each led by its first line.
struct Row {
  AxisAlignedBox lead;
  std::vector<AxisAlignedBox> members;
};

// The column whose x-interval holds the centre, else the nearest one.
int column_of(const std::vector<std::pair<int, int>>& columns, int center) {
  int nearest = -1;
  int64_t nearest_distance = std::numeric_limits<int64_t>::max();
  for (size_t index = 0; index < columns.size(); ++index) {
    const auto [left, right] = columns[index];
    if (left > right) continue;
    if (center >= left && center <= right) return static_cast<int>(index);
    const int64_t distance =
        center < left ? static_cast<int64_t>(left) - center : static_cast<int64_t>(center) - right;
    if (distance < nearest_distance) {
      nearest = static_cast<int>(index);
      nearest_distance = distance;
    }
  }
  return nearest;
}

// One cell per row and column the rows' lines fall in, numbered on from
// first_row; reached marks the columns that got a line.
std::vector<StructuredCell> place_rows(const std::vector<Row>& built, const std::vector<std::pair<int, int>>& columns,
                                       int first_row, std::vector<bool>& reached) {
  std::vector<StructuredCell> extension;
  for (size_t index = 0; index < built.size(); ++index) {
    std::map<int, size_t> row_cells;
    for (const auto& box : built[index].members) {
      const int column = column_of(columns, static_cast<int>((static_cast<int64_t>(box.left) + box.right) / 2));
      if (column < 0) continue;
      reached[static_cast<size_t>(column)] = true;
      if (const auto taken = row_cells.find(column); taken != row_cells.end()) {
        auto& cell = extension[taken->second];
        cell.left = std::min(cell.left, box.left);
        cell.top = std::min(cell.top, box.top);
        cell.right = std::max(cell.right, box.right);
        cell.bottom = std::max(cell.bottom, box.bottom);
        continue;
      }
      row_cells.emplace(column, extension.size());
      extension.push_back(
          {first_row + static_cast<int>(index), column, 1, 1, false, box.left, box.top, box.right, box.bottom});
    }
  }
  return extension;
}

}  // namespace

void extend_truncated_table(LayoutRegion& table, const std::vector<OcrLine>& lines) {
  if (table.structured_cells.empty()) return;
  int rows = 0;
  int cols = 0;
  int cut = 0;
  for (const auto& cell : table.structured_cells) {
    rows = std::max(rows, cell.row + cell.row_span);
    cols = std::max(cols, cell.col + cell.col_span);
    cut = std::max(cut, cell.bottom);
  }
  // Each column's x-interval from the words the model's own rows put in it,
  // not from its cell boxes, which on some tables sit tens of pixels off
  // their words. A column with no single-column word keeps its box extent.
  OcrPage page;
  page.lines = lines;
  page.regions = {table};
  const TableLineAssignment assignment = assign_table_lines(page, page.regions[0]);
  std::vector<std::pair<int, int>> words(static_cast<size_t>(cols),
                                         {std::numeric_limits<int>::max(), std::numeric_limits<int>::min()});
  std::vector<std::pair<int, int>> boxes = words;
  // Columns the model's body rows hold words in; a header-only column or a
  // spacer never gets data rows and says nothing about the cut.
  std::vector<bool> body_used(static_cast<size_t>(cols), false);
  for (const auto& cell : table.structured_cells) {
    if (cell.col_span != 1) continue;
    auto& column = boxes[static_cast<size_t>(cell.col)];
    column.first = std::min(column.first, cell.left);
    column.second = std::max(column.second, cell.right);
  }
  for (size_t index = 0; index < lines.size(); ++index) {
    const int at = assignment.cell_of_line[index];
    if (at < 0) continue;
    const auto& cell = table.structured_cells[static_cast<size_t>(at)];
    if (cell.col_span != 1) continue;
    const AxisAlignedBox box = bounding_box(lines[index]);
    auto& column = words[static_cast<size_t>(cell.col)];
    column.first = std::min(column.first, box.left);
    column.second = std::max(column.second, box.right);
    if (!cell.header) body_used[static_cast<size_t>(cell.col)] = true;
  }
  // A label column filled on every body row must be reached below the
  // cut. A group-label column (one label over a run of min/avg/max rows)
  // stays empty on most rows, so the rows past the cut need not reach it
  // and it does not count among the columns that must be reached.
  std::map<int, bool> body_rows;
  for (size_t index = 0; index < lines.size(); ++index) {
    const int at = assignment.cell_of_line[index];
    if (at < 0) continue;
    const auto& cell = table.structured_cells[static_cast<size_t>(at)];
    if (cell.header) continue;
    body_rows[cell.row] = body_rows[cell.row] || (cell.col == 0 && cell.col_span == 1);
  }
  const bool label_required = !body_rows.empty() && std::ranges::all_of(body_rows, [](const auto& row) {
    return row.second;
  });
  std::vector<std::pair<int, int>> word_columns(static_cast<size_t>(cols));
  for (size_t index = 0; index < word_columns.size(); ++index) {
    word_columns[index] = words[index].first <= words[index].second ? words[index] : boxes[index];
  }

  // The lines inside the table whose centre lies under every model cell.
  std::vector<AxisAlignedBox> below;
  for (const auto& line : lines) {
    if (line.text.empty() || line.polygon.empty()) continue;
    const AxisAlignedBox box = bounding_box(line);
    const int64_t center_x = (static_cast<int64_t>(box.left) + box.right) / 2;
    const int64_t center_y = (static_cast<int64_t>(box.top) + box.bottom) / 2;
    if (center_x < table.left || center_x > table.right || center_y > table.bottom || center_y <= cut) {
      continue;
    }
    below.push_back(box);
  }
  if (below.empty()) return;
  std::vector<int64_t> heights;
  for (const auto& box : below) heights.push_back(box.height());
  std::ranges::nth_element(heights, heights.begin() + static_cast<std::ptrdiff_t>(heights.size() / 2));
  const int64_t median_height = heights[heights.size() / 2];
  std::ranges::sort(below, [](const AxisAlignedBox& a, const AxisAlignedBox& b) {
    return a.top != b.top ? a.top < b.top : a.left < b.left;
  });

  // Rows from ordinary lines: a line joins the row whose first line holds
  // its centre. A line over 1.5 median heights (a label set across two
  // rows) joins the row its top line belongs to, so it never fuses two.
  std::vector<Row> built;
  std::vector<AxisAlignedBox> tall;
  for (const auto& box : below) {
    if (box.height() * 2 > median_height * 3) {
      tall.push_back(box);
      continue;
    }
    const int64_t center_y = (static_cast<int64_t>(box.top) + box.bottom) / 2;
    if (built.empty() || center_y > built.back().lead.bottom) {
      built.push_back({box, {}});
    }
    built.back().members.push_back(box);
  }
  if (built.empty()) return;
  for (const auto& box : tall) {
    const int64_t anchor = static_cast<int64_t>(box.top) + median_height / 2;
    size_t best = 0;
    int64_t best_distance = std::numeric_limits<int64_t>::max();
    for (size_t index = 0; index < built.size(); ++index) {
      const auto& lead = built[index].lead;
      const int64_t distance = anchor < lead.top ? lead.top - anchor : anchor > lead.bottom ? anchor - lead.bottom : 0;
      if (distance < best_distance) {
        best = index;
        best_distance = distance;
      }
    }
    built[best].members.push_back(box);
  }

  // The model's body columns must get words below the cut too: the
  // row-label column when it is filled on every row, and all but a quarter
  // of the rest (a "best" marker or a sparse column can stay empty). The
  // word intervals go first; where the model's cells hold several numbers
  // each those intervals overlap and miss, so the cell boxes get a turn.
  // Missing more means the intervals miss the words, and wrong numbers in
  // wrong columns are worse than rows without columns: each row past the
  // cut is then one cell across the table, its words kept out of the body.
  for (const auto* columns : {&word_columns, &boxes}) {
    std::vector<bool> reached(static_cast<size_t>(cols), false);
    std::vector<StructuredCell> extension = place_rows(built, *columns, rows, reached);
    size_t used_count = 0;
    size_t missed = 0;
    for (size_t index = label_required ? 0 : 1; index < body_used.size(); ++index) {
      if (!body_used[index]) continue;
      ++used_count;
      if (!reached[index]) ++missed;
    }
    const bool label_missed = label_required && body_used[0] && !reached[0];
    if (!label_missed && missed * 4 <= used_count) {
      table.structured_cells.insert(table.structured_cells.end(), extension.begin(), extension.end());
      return;
    }
  }
  for (size_t index = 0; index < built.size(); ++index) {
    AxisAlignedBox row_box = built[index].members.front();
    for (const auto& box : built[index].members) {
      row_box.left = std::min(row_box.left, box.left);
      row_box.top = std::min(row_box.top, box.top);
      row_box.right = std::max(row_box.right, box.right);
      row_box.bottom = std::max(row_box.bottom, box.bottom);
    }
    table.structured_cells.push_back({rows + static_cast<int>(index), 0, 1, cols, false, row_box.left,
                                      row_box.top, row_box.right, row_box.bottom});
  }
}

TableLineAssignment assign_table_lines(const OcrPage& page, const LayoutRegion& table) {
  TableLineAssignment out;
  out.cell_of_line.assign(page.lines.size(), -1);
  const auto& cells = table.structured_cells;
  std::vector<size_t> members;
  for (size_t index = 0; index < page.lines.size(); ++index) {
    const auto& line = page.lines[index];
    if (line.text.empty() || line.polygon.empty()) continue;
    if (region_for_line(page, line) == &table) members.push_back(index);
  }
  if (cells.empty()) return out;
  out.structured = true;
  for (const size_t index : members) {
    const AxisAlignedBox box = bounding_box(page.lines[index]);
    int chosen = -1;
    for (size_t at = 0; at < cells.size() && chosen < 0; ++at) {
      if (center_inside(box, cells[at])) chosen = static_cast<int>(at);
    }
    if (chosen < 0) {
      int64_t best_area = 0;
      for (size_t at = 0; at < cells.size(); ++at) {
        const int64_t shared = overlap_area(box, cells[at]);
        if (shared > best_area) {
          best_area = shared;
          chosen = static_cast<int>(at);
        }
      }
    }
    if (chosen < 0) {
      // Only a cell of the line's own row band: no further below or above
      // than the line is tall. A stray line rows away from every cell (a
      // total under the last row) stays body text rather than polluting
      // the nearest amount.
      std::pair<int64_t, int64_t> best{box.height() + 1, 0};
      for (size_t at = 0; at < cells.size(); ++at) {
        const auto& cell = cells[at];
        const std::pair<int64_t, int64_t> gap{axis_gap(box.top, box.bottom, cell.top, cell.bottom),
                                              axis_gap(box.left, box.right, cell.left, cell.right)};
        if (gap < best) {
          best = gap;
          chosen = static_cast<int>(at);
        }
      }
    }
    out.cell_of_line[index] = chosen;
  }
  return out;
}

}  // namespace grparse
