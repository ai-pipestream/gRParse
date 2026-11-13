#include "grparse/assembly_table_data.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "grparse/reading_order.h"
#include "grparse/region_geometry.h"
#include "grparse/table_structure.h"

namespace pipestream = ai::pipestream;

namespace grparse {

void set_bounding_box(const AxisAlignedBox& box, pipestream::document::v1::BoundingBox* output) {
  output->set_l(box.left);
  output->set_t(box.top);
  output->set_r(box.right);
  output->set_b(box.bottom);
  output->set_coord_origin(pipestream::document::v1::COORD_ORIGIN_TOPLEFT);
}

// Model table structure (D3): the recognized cells carry real spans and
// header rows. Each line bound to the table lands in exactly the cell
// assign_table_lines gives it, words in visual line order, so overlapping
// model boxes never duplicate text and a line the boxes miss still lands in
// the nearest cell instead of leaking out as body text. The flat cell list
// holds each cell once while the row grid repeats spanning cells across
// every position they cover, with empty unit cells filling positions no
// recognized cell claims.
void fill_structured_table_data(const OcrPage& page, const LayoutRegion& region,
                                const TableLineAssignment& assignment,
                                pipestream::document::v1::TableData* data) {
  int rows = 0;
  int cols = 0;
  for (const auto& cell : region.structured_cells) {
    rows = std::max(rows, cell.row + cell.row_span);
    cols = std::max(cols, cell.col + cell.col_span);
  }
  data->set_num_rows(rows);
  data->set_num_cols(cols);

  std::vector<std::vector<size_t>> cell_lines(region.structured_cells.size());
  for (size_t index = 0; index < assignment.cell_of_line.size(); ++index) {
    const int cell = assignment.cell_of_line[index];
    if (cell >= 0) cell_lines[static_cast<size_t>(cell)].push_back(index);
  }

  std::vector<int> owner(static_cast<size_t>(rows) * static_cast<size_t>(cols), -1);
  std::vector<pipestream::document::v1::TableCell> protos;
  protos.reserve(region.structured_cells.size());
  for (size_t cell_at = 0; cell_at < region.structured_cells.size(); ++cell_at) {
    const auto& cell = region.structured_cells[cell_at];
    pipestream::document::v1::TableCell proto_cell;
    proto_cell.set_row_span(cell.row_span);
    proto_cell.set_col_span(cell.col_span);
    proto_cell.set_start_row_offset_idx(cell.row);
    proto_cell.set_end_row_offset_idx(cell.row + cell.row_span);
    proto_cell.set_start_col_offset_idx(cell.col);
    proto_cell.set_end_col_offset_idx(cell.col + cell.col_span);
    proto_cell.set_column_header(cell.header);
    std::string text;
    for (const size_t line_index : visual_line_order(page, std::move(cell_lines[cell_at]))) {
      if (!text.empty()) text.push_back(' ');
      text += page.lines[line_index].text;
    }
    proto_cell.set_text(std::move(text));
    AxisAlignedBox box{cell.left, cell.top, cell.right, cell.bottom};
    set_bounding_box(box, proto_cell.mutable_bbox());
    const int cell_index = static_cast<int>(protos.size());
    for (int row = cell.row; row < cell.row + cell.row_span && row < rows; ++row) {
      for (int col = cell.col; col < cell.col + cell.col_span && col < cols; ++col) {
        auto& slot = owner[static_cast<size_t>(row) * cols + col];
        if (slot < 0) slot = cell_index;
      }
    }
    *data->add_table_cells() = proto_cell;
    protos.push_back(std::move(proto_cell));
  }
  for (int row = 0; row < rows; ++row) {
    auto* grid_row = data->add_grid();
    for (int col = 0; col < cols; ++col) {
      const int cell_index = owner[static_cast<size_t>(row) * cols + col];
      if (cell_index >= 0) {
        *grid_row->add_cells() = protos[static_cast<size_t>(cell_index)];
      } else {
        auto* blank = grid_row->add_cells();
        blank->set_row_span(1);
        blank->set_col_span(1);
        blank->set_start_row_offset_idx(row);
        blank->set_end_row_offset_idx(row + 1);
        blank->set_start_col_offset_idx(col);
        blank->set_end_col_offset_idx(col + 1);
      }
    }
  }
}

// Geometry table structure (D2 v0): every grid position becomes a TableCell
// with unit spans, mirrored into both the flat cell list and the row grid.
// Header flags stay false; geometry cannot tell a header from a body row.
void fill_table_data(const OcrPage& page, const LayoutRegion& region,
                     pipestream::document::v1::TableData* data) {
  const TableLineAssignment assignment = assign_table_lines(page, region);
  if (assignment.structured) {
    fill_structured_table_data(page, region, assignment, data);
    return;
  }
  const TableGrid grid = build_table_grid(page, region);
  data->set_num_rows(grid.rows);
  data->set_num_cols(grid.cols);
  std::vector<pipestream::document::v1::TableRow*> rows;
  rows.reserve(static_cast<size_t>(grid.rows));
  for (int row = 0; row < grid.rows; ++row) rows.push_back(data->add_grid());
  for (const auto& cell : grid.cells) {
    pipestream::document::v1::TableCell proto_cell;
    proto_cell.set_row_span(1);
    proto_cell.set_col_span(1);
    proto_cell.set_start_row_offset_idx(cell.row);
    proto_cell.set_end_row_offset_idx(cell.row + 1);
    proto_cell.set_start_col_offset_idx(cell.col);
    proto_cell.set_end_col_offset_idx(cell.col + 1);
    std::string text;
    for (const size_t line_index : cell.line_indices) {
      if (!text.empty()) text.push_back(' ');
      text += page.lines[line_index].text;
    }
    proto_cell.set_text(std::move(text));
    if (!cell.line_indices.empty()) set_bounding_box(cell.box, proto_cell.mutable_bbox());
    *data->add_table_cells() = proto_cell;
    *rows[static_cast<size_t>(cell.row)]->add_cells() = std::move(proto_cell);
  }
}

}  // namespace grparse
