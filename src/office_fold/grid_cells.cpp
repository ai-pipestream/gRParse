#include "grparse/office_fold/grid_cells.h"

#include <cctype>
#include <cstdint>
#include <set>
#include <utility>

namespace grparse::office_fold {

namespace {

// Copies one placed cell onto the grid slot it anchors at. The slot was
// created blank, so everything the cell says has to be carried over.
void fill_slot(const docv1::TableCell& cell, docv1::TableCell* slot) {
  slot->set_text(cell.text());
  slot->set_row_span(cell.row_span());
  slot->set_col_span(cell.col_span());
  slot->set_end_row_offset_idx(cell.end_row_offset_idx());
  slot->set_end_col_offset_idx(cell.end_col_offset_idx());
  slot->set_column_header(cell.column_header());
  slot->set_row_header(cell.row_header());
  slot->set_row_section(cell.row_section());
  if (cell.has_value()) *slot->mutable_value() = cell.value();
  if (cell.has_bbox()) *slot->mutable_bbox() = cell.bbox();
}

// The blank base grid: one single-cell slot per position.
void add_blank_grid(docv1::TableData* data, int rows, int columns) {
  for (int row = 0; row < rows; row++) {
    docv1::TableRow* out_row = data->add_grid();
    for (int column = 0; column < columns; column++) {
      docv1::TableCell* out = out_row->add_cells();
      out->set_start_row_offset_idx(row);
      out->set_end_row_offset_idx(row + 1);
      out->set_start_col_offset_idx(column);
      out->set_end_col_offset_idx(column + 1);
      out->set_row_span(1);
      out->set_col_span(1);
    }
  }
}

}  // namespace

std::string column_name(int column) {
  std::string name;
  for (int c = column; c >= 0; c = c / 26 - 1) {
    name.insert(name.begin(), static_cast<char>('A' + c % 26));
  }
  return name;
}

bool anchor_of_cell_name(const std::string& name, int* row, int* column) {
  size_t pos = 0;
  long col = 0;
  while (pos < name.size()
         && std::isupper(static_cast<unsigned char>(name[pos]))) {
    col = col * 26 + (name[pos] - 'A' + 1);
    pos++;
  }
  if (pos == 0 || pos >= name.size()) return false;
  long row_number = 0;
  size_t digit = pos;
  for (; digit < name.size()
         && std::isdigit(static_cast<unsigned char>(name[digit]));
       digit++) {
    row_number = row_number * 10 + (name[digit] - '0');
  }
  if (digit == pos || row_number <= 0) return false;
  *row = static_cast<int>(row_number - 1);
  *column = static_cast<int>(col - 1);
  return true;
}

void set_grid_span(const officev1::SheetRangeRef& range,
                   const std::string& sheet, docv1::GridSpan* out) {
  docv1::GridCell* start = out->mutable_start();
  start->set_row(range.start_row());
  start->set_col(range.start_column());
  docv1::GridCell* end = out->mutable_end();
  end->set_row(range.end_row());
  end->set_col(range.end_column());
  if (sheet.empty()) return;
  start->set_sheet(sheet);
  end->set_sheet(sheet);
}

bool numeric_display(const std::string& text) {
  size_t i = 0;
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) i++;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) i++;
  int digits = 0;
  bool fraction = false;
  for (; i < text.size(); i++) {
    const char c = text[i];
    if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
      digits++;
    } else if ((c == '.' || c == ',') && !fraction && i + 1 < text.size()) {
      if (c == '.') fraction = true;
    } else {
      break;
    }
  }
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) i++;
  return digits > 0 && i == text.size();
}

bool quantity_cell(const docv1::TableCell& cell) {
  if (!cell.has_value()) return false;
  switch (cell.value().kind_case()) {
    case docv1::CellValue::kNumber:
    case docv1::CellValue::kBoolean:
    case docv1::CellValue::kDatetime:
      return true;
    case docv1::CellValue::kFormula:
      return numeric_display(cell.text());
    default:
      return false;
  }
}

bool label_cell(const docv1::TableCell& cell) {
  if (cell.has_value() && cell.value().kind_case() != docv1::CellValue::KIND_NOT_SET) {
    return false;
  }
  for (const char c : cell.text()) {
    if (std::isspace(static_cast<unsigned char>(c)) == 0) return true;
  }
  return false;
}

docv1::TableCell* place_cell(docv1::TableData* data, int row, int column,
                             const std::string& text) {
  docv1::TableCell* cell = data->add_table_cells();
  cell->set_start_row_offset_idx(row);
  cell->set_end_row_offset_idx(row + 1);
  cell->set_start_col_offset_idx(column);
  cell->set_end_col_offset_idx(column + 1);
  cell->set_row_span(1);
  cell->set_col_span(1);
  cell->set_text(text);
  return cell;
}

void fill_grid_from_cells(docv1::TableData* data) {
  const int rows = data->num_rows();
  const int columns = data->num_cols();
  // 64-bit product: adversarial row and column counts must saturate the
  // guard, not overflow it into acceptance.
  if (rows <= 0 || columns <= 0 ||
      static_cast<int64_t>(rows) * columns > kMaxGridCells) {
    return;
  }
  add_blank_grid(data, rows, columns);
  // Several cells can anchor at one slot when a base cell was split, so
  // the first cell placed there keeps it: the base cell is emitted before
  // the pieces split out of it.
  std::set<std::pair<int, int>> filled;
  for (const docv1::TableCell& cell : data->table_cells()) {
    // Merged or irregular office tables can report cells beyond the
    // declared grid; those stay in table_cells but have no grid slot.
    if (cell.start_row_offset_idx() >= data->grid_size()) continue;
    docv1::TableRow* out_row = data->mutable_grid(cell.start_row_offset_idx());
    if (cell.start_col_offset_idx() >= out_row->cells_size()) continue;
    if (!filled.insert({cell.start_row_offset_idx(),
                        cell.start_col_offset_idx()}).second) {
      continue;
    }
    fill_slot(cell, out_row->mutable_cells(cell.start_col_offset_idx()));
  }
}

}  // namespace grparse::office_fold
