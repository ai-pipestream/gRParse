#include "storage_table.h"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "storage_node.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse::confluence {
namespace {

// A span larger than this is malformed markup rather than a wide cell.
constexpr int kMaxSpan = 1024;

}  // namespace

void collect_rows(const Node& node, bool head, std::vector<TableRowNode>* rows) {
  for (const Node& child : node.children) {
    if (html_is(child, "tr")) {
      rows->push_back({&child, head});
      continue;
    }
    if (html_is(child, "thead")) {
      collect_rows(child, true, rows);
      continue;
    }
    if (html_is(child, "tbody") || html_is(child, "tfoot")) {
      collect_rows(child, false, rows);
      continue;
    }
    if (!child.text_node) collect_rows(child, head, rows);
  }
}

int span_attribute(const Node& cell, std::string_view name, bool* clamped) {
  const std::string* raw = attribute(cell, "", name);
  if (raw == nullptr) return 1;
  int value = 0;
  for (const char digit : *raw) {
    if (std::isdigit(static_cast<unsigned char>(digit)) == 0) return 1;
    value = value * 10 + (digit - '0');
    if (value > kMaxSpan) {
      *clamped = true;
      return kMaxSpan;
    }
  }
  return value > 0 ? value : 1;
}

void reserve_slots(std::vector<std::vector<bool>>* occupied, int row, int column,
                   int row_span, int col_span) {
  for (int r = row; r < row + row_span; ++r) {
    auto& target = (*occupied)[static_cast<size_t>(r)];
    if (target.size() < static_cast<size_t>(column + col_span)) {
      target.resize(static_cast<size_t>(column + col_span), false);
    }
    for (int c = column; c < column + col_span; ++c) {
      target[static_cast<size_t>(c)] = true;
    }
  }
}

void materialize_grid(docv1::TableData* data, int num_rows, int num_cols) {
  for (int row = 0; row < num_rows; ++row) {
    docv1::TableRow* out_row = data->add_grid();
    for (int column = 0; column < num_cols; ++column) {
      docv1::TableCell* slot = out_row->add_cells();
      slot->set_start_row_offset_idx(row);
      slot->set_end_row_offset_idx(row + 1);
      slot->set_start_col_offset_idx(column);
      slot->set_end_col_offset_idx(column + 1);
      slot->set_row_span(1);
      slot->set_col_span(1);
    }
  }
  for (const docv1::TableCell& cell : data->table_cells()) {
    for (int row = cell.start_row_offset_idx(); row < cell.end_row_offset_idx();
         ++row) {
      if (row < 0 || row >= data->grid_size()) continue;
      docv1::TableRow* out_row = data->mutable_grid(row);
      for (int column = cell.start_col_offset_idx();
           column < cell.end_col_offset_idx(); ++column) {
        if (column < 0 || column >= out_row->cells_size()) continue;
        *out_row->mutable_cells(column) = cell;
      }
    }
  }
}

}  // namespace grparse::confluence
