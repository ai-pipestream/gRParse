#include "grparse/office_fold/sheet_header_band.h"

#include <algorithm>
#include <cctype>
#include <iterator>

#include "grparse/office_fold/grid_cells.h"

namespace grparse::office_fold {

namespace {

// A cell that holds something: text, or a typed value.
bool filled_cell(const docv1::TableCell& cell) {
  if (cell.has_value()
      && cell.value().kind_case() != docv1::CellValue::KIND_NOT_SET) {
    return true;
  }
  return std::ranges::any_of(cell.text(), [](unsigned char c) {
    return std::isspace(c) == 0;
  });
}

using Rows = std::vector<SheetRowShape>;

// The first data row: a quantity-bearing row at least half as wide as the
// widest one. Returns rows.end() when no row carries a quantity.
Rows::const_iterator first_data_row(const Rows& rows, int* width) {
  *width = 0;
  for (const SheetRowShape& row : rows) {
    if (row.quantities > 0) *width = std::max(*width, row.coverage);
  }
  return std::ranges::find_if(rows, [width](const SheetRowShape& row) {
    return row.quantities > 0 && row.coverage * 2 >= *width;
  });
}

// The nearest contiguous run of wide label rows above `data`, ascending.
Rows nearest_label_run(const Rows& rows, Rows::const_iterator data,
                       int width) {
  Rows run;
  auto it = std::make_reverse_iterator(data);
  while (it != rows.rend() && !it->wide_labels(width)) ++it;
  for (; it != rows.rend() && it->wide_labels(width); ++it) {
    if (!run.empty() && it->index + 1 != run.front().index) break;
    run.insert(run.begin(), *it);
  }
  return run;
}

// Header lines get denser going down: a line joins the band under the
// line above when it holds more labels, or when the line above is a group
// line. The band stops at the first line that does neither.
std::vector<int> band_prefix(const Rows& run) {
  std::vector<int> band;
  if (run.empty()) return band;
  band.push_back(run.front().index);
  for (size_t i = 1; i < run.size() && band.size() < kMaxHeaderBandRows; i++) {
    const SheetRowShape& above = run[i - 1];
    if (run[i].labels <= above.labels && !above.group_line) break;
    band.push_back(run[i].index);
  }
  return band;
}

// A sheet without quantities: the first row is the header only when it is a
// full-width label row (a group line may carry one leaf line under it) over
// at least two consecutive label rows.
std::vector<int> text_band(const Rows& rows) {
  int width = 0;
  for (const SheetRowShape& row : rows) width = std::max(width, row.coverage);
  const SheetRowShape& first = rows.front();
  if (!first.wide_labels(width) || first.coverage < width) return {};
  Rows run{first};
  if (rows.size() > 1 && rows[1].index == first.index + 1
      && rows[1].wide_labels(width)) {
    run.push_back(rows[1]);
  }
  std::vector<int> band = band_prefix(run);
  const size_t records = band.size();
  if (rows.size() < records + 2) return {};
  for (size_t i = records; i < records + 2; i++) {
    if (rows[i].labels < 2) return {};
    if (rows[i].index != band.back() + static_cast<int>(i - records) + 1) {
      return {};
    }
  }
  return band;
}

}  // namespace

SheetRowShape sheet_row_shape(int index,
                              const std::vector<docv1::TableCell*>& cells) {
  SheetRowShape shape;
  shape.index = index;
  for (const docv1::TableCell* cell : cells) {
    if (!filled_cell(*cell)) continue;
    const int span = std::max(1, cell->col_span());
    shape.coverage += span;
    if (quantity_cell(*cell)) shape.quantities++;
    if (!label_cell(*cell)) continue;
    shape.labels++;
    if (span >= 2) shape.group_line = true;
  }
  return shape;
}

// The band's densest line must name more columns than the first data row
// fills with labels: a header names the quantity columns, a record that
// merely lacks its numbers does not.
bool denser_than(const Rows& run, const std::vector<int>& band,
                 const SheetRowShape& data) {
  int labels = 0;
  for (const SheetRowShape& row : run) {
    if (std::ranges::find(band, row.index) != band.end()) {
      labels = std::max(labels, row.labels);
    }
  }
  return labels > data.labels;
}

std::vector<int> header_band_rows(const Rows& rows) {
  if (rows.size() < 2) return {};
  int width = 0;
  const auto data = first_data_row(rows, &width);
  if (data == rows.end()) return text_band(rows);
  const Rows run = nearest_label_run(rows, data, width);
  std::vector<int> band = band_prefix(run);
  if (!band.empty() && !denser_than(run, band, *data)) band.clear();
  return band;
}

}  // namespace grparse::office_fold
