#include "grparse/table_column_anchor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace grparse {
namespace {

// The code points a table value is written with besides digits: separators,
// signs, percent and significance marks, brackets and comparison signs.
bool value_code_point(char32_t point) {
  if (point >= U'0' && point <= U'9') return true;
  switch (point) {
    case U' ': case U'\t': case U'.': case U',': case U'%': case U'*': case U'+': case U'-':
    case U'(': case U')': case U'[': case U']': case U'/': case U'<': case U'>': case U'=':
    case U'~': case U'−': case U'±': case U'†': case U'‡':
      return true;
    default:
      return false;
  }
}

// Decodes one UTF-8 code point at `at` and advances it; a malformed byte
// reads as itself so the walk always moves on.
char32_t next_code_point(std::string_view text, size_t& at) {
  const auto byte = static_cast<unsigned char>(text[at]);
  size_t length = 1;
  char32_t point = byte;
  if (byte >= 0xF0) {
    length = 4;
    point = byte & 0x07U;
  } else if (byte >= 0xE0) {
    length = 3;
    point = byte & 0x0FU;
  } else if (byte >= 0xC0) {
    length = 2;
    point = byte & 0x1FU;
  }
  if (at + length > text.size()) length = 1;
  for (size_t index = 1; index < length; ++index) {
    point = (point << 6U) | (static_cast<unsigned char>(text[at + index]) & 0x3FU);
  }
  at += length;
  return length == 1 ? byte : point;
}

bool has_digit(std::string_view text) {
  return std::ranges::any_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

// A cell value: digits written with value marks only ("0.0384", "32.29 %",
// "0.85±0.02"), never a label that merely holds a digit ("Acc-7(%)").
bool is_value(std::string_view text) {
  if (!has_digit(text)) return false;
  for (size_t at = 0; at < text.size();) {
    if (!value_code_point(next_code_point(text, at))) return false;
  }
  return true;
}

double center_x(const AxisAlignedBox& box) { return (static_cast<double>(box.left) + box.right) / 2.0; }

// Leading rows whose lines are mostly not values: the column headings.
std::set<int> heading_rows(const OcrPage& page, const std::vector<StructuredCell>& cells,
                           const std::vector<int>& cell_of_line) {
  std::map<int, std::pair<size_t, size_t>> rows;  // row -> (values, lines)
  for (size_t index = 0; index < cell_of_line.size(); ++index) {
    if (cell_of_line[index] < 0) continue;
    auto& [values, total] = rows[cells[static_cast<size_t>(cell_of_line[index])].row];
    ++total;
    if (is_value(page.lines[index].text)) ++values;
  }
  std::set<int> headings;
  for (const auto& [row, counts] : rows) {
    if (counts.first * 2 >= counts.second) break;
    headings.insert(row);
  }
  return headings;
}

// Horizontal extents merged across gaps no wider than the gutter.
std::vector<std::pair<int, int>> merged_bands(std::vector<std::pair<int, int>> spans, int gutter) {
  std::ranges::sort(spans);
  std::vector<std::pair<int, int>> bands;
  for (const auto& [left, right] : spans) {
    if (!bands.empty() && left <= bands.back().second + gutter) {
      bands.back().second = std::max(bands.back().second, right);
    } else {
      bands.emplace_back(left, right);
    }
  }
  return bands;
}

int cell_covering(const std::vector<StructuredCell>& cells, int row, int col) {
  for (size_t at = 0; at < cells.size(); ++at) {
    const auto& cell = cells[at];
    if (row >= cell.row && row < cell.row + cell.row_span && col >= cell.col && col < cell.col + cell.col_span) {
      return static_cast<int>(at);
    }
  }
  return -1;
}

template <typename T>
T median_of(std::vector<T> values) {
  std::ranges::nth_element(values, values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2));
  return values[values.size() / 2];
}

}  // namespace

void anchor_body_columns(const OcrPage& page, const LayoutRegion& table, std::vector<int>& cell_of_line) {
  const auto& cells = table.structured_cells;
  if (cells.empty()) return;
  const std::set<int> headings = heading_rows(page, cells, cell_of_line);
  if (headings.empty()) return;
  const auto cell_of = [&](size_t line) -> const StructuredCell& {
    return cells[static_cast<size_t>(cell_of_line[line])];
  };

  std::vector<size_t> body;
  size_t numeric = 0;
  for (size_t index = 0; index < cell_of_line.size(); ++index) {
    if (cell_of_line[index] < 0 || headings.contains(cell_of(index).row)) continue;
    body.push_back(index);
    if (has_digit(page.lines[index].text)) ++numeric;
  }
  if (body.size() < 4 || numeric * 2 < body.size()) return;

  const int64_t width = static_cast<int64_t>(table.right) - table.left;
  std::vector<size_t> narrow;
  for (const size_t index : body) {
    const AxisAlignedBox box = bounding_box(page.lines[index]);
    if (cell_of(index).col_span == 1 && box.width() * 100 < width * 35) narrow.push_back(index);
  }
  if (narrow.size() < 4) return;

  std::vector<int64_t> heights;
  std::vector<std::pair<int, int>> spans;
  for (const size_t index : narrow) {
    const AxisAlignedBox box = bounding_box(page.lines[index]);
    heights.push_back(box.height());
    spans.emplace_back(box.left, box.right);
  }
  const int gutter = std::max<int>(2, static_cast<int>(median_of(heights) / 2));
  const auto bands = merged_bands(std::move(spans), gutter);

  std::vector<int64_t> widths;
  for (const auto& cell : cells) {
    if (cell.col_span == 1 && cell.right > cell.left) widths.push_back(static_cast<int64_t>(cell.right) - cell.left);
  }
  const int64_t limit = widths.empty() ? width : 2 * median_of(widths);

  // Heading words over a column say where it is, unless their cell is
  // wider than two columns.
  std::vector<std::pair<double, int>> anchors;
  for (size_t index = 0; index < cell_of_line.size(); ++index) {
    if (cell_of_line[index] < 0) continue;
    const auto& cell = cell_of(index);
    if (!headings.contains(cell.row) || cell.col_span != 1) continue;
    if (static_cast<int64_t>(cell.right) - cell.left > limit) continue;
    anchors.emplace_back(center_x(bounding_box(page.lines[index])), cell.col);
  }

  const auto band_of = [&](size_t line) -> size_t {
    const double center = center_x(bounding_box(page.lines[line]));
    for (size_t at = 0; at < bands.size(); ++at) {
      if (center >= bands[at].first && center <= bands[at].second) return at;
    }
    return bands.size();
  };
  std::vector<std::vector<int>> votes(bands.size());
  for (const size_t index : narrow) {
    if (const size_t at = band_of(index); at < bands.size()) votes[at].push_back(cell_of(index).col);
  }

  // A band is one visual column. When the model put all its rows in one
  // column it is left alone; when it split them, the heading over the band
  // decides if it is one of the columns the rows went to, else the column
  // most of them went to.
  std::vector<std::optional<int>> band_column(bands.size());
  for (size_t at = 0; at < bands.size(); ++at) {
    const auto& [left, right] = bands[at];
    const auto& band_votes = votes[at];
    if (band_votes.empty() || static_cast<int64_t>(right) - left > limit) continue;
    std::map<int, size_t> counts;
    for (const int col : band_votes) ++counts[col];
    if (counts.size() == 1) continue;
    std::set<int> headed;
    for (const auto& [x, col] : anchors) {
      if (x >= left - gutter && x <= right + gutter) headed.insert(col);
    }
    if (headed.size() == 1 && counts.contains(*headed.begin())) {
      band_column[at] = *headed.begin();
      continue;
    }
    const auto top = std::ranges::max_element(counts, [](const auto& a, const auto& b) { return a.second < b.second; });
    if (top->second * 2 > band_votes.size()) band_column[at] = top->first;
  }

  for (const size_t index : narrow) {
    const size_t at = band_of(index);
    if (at >= bands.size() || !band_column[at].has_value()) continue;
    const auto& cell = cell_of(index);
    if (*band_column[at] == cell.col) continue;
    const int target = cell_covering(cells, cell.row, *band_column[at]);
    if (target < 0) continue;
    const auto& destination = cells[static_cast<size_t>(target)];
    if (destination.col_span == 1 && !headings.contains(destination.row)) cell_of_line[index] = target;
  }
}

}  // namespace grparse
