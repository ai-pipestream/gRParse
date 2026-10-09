#include "grparse/assembly_table_data.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

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
// header rows.  Lines bound to the table land in the first cell whose box
// contains their center; the flat cell list holds each cell once while the
// row grid repeats spanning cells across every position they cover, with
// empty unit cells filling positions no recognized cell claims.
void fill_structured_table_data(const OcrPage& page, const LayoutRegion& region,
                                pipestream::document::v1::TableData* data) {
  int rows = 0;
  int cols = 0;
  for (const auto& cell : region.structured_cells) {
    rows = std::max(rows, cell.row + cell.row_span);
    cols = std::max(cols, cell.col + cell.col_span);
  }
  data->set_num_rows(rows);
  data->set_num_cols(cols);

  struct MemberLine {
    size_t index = 0;
    AxisAlignedBox box;
  };
  std::vector<MemberLine> lines;
  for (size_t index = 0; index < page.lines.size(); ++index) {
    const auto& line = page.lines[index];
    if (line.text.empty() || line.polygon.empty()) continue;
    if (region_for_line(page, line) == &region) lines.push_back({index, bounding_box(line)});
  }
  std::ranges::sort(lines, [](const MemberLine& a, const MemberLine& b) {
    if (a.box.top != b.box.top) return a.box.top < b.box.top;
    return a.box.left < b.box.left;
  });

  std::vector<int> owner(static_cast<size_t>(rows) * static_cast<size_t>(cols), -1);
  // Each line belongs to exactly one cell: the first whose box contains its
  // center, matching table_claimed_lines. Overlapping model boxes must not
  // duplicate the same text into two cells.
  std::vector<bool> line_taken(lines.size(), false);
  std::vector<pipestream::document::v1::TableCell> protos;
  protos.reserve(region.structured_cells.size());
  for (const auto& cell : region.structured_cells) {
    pipestream::document::v1::TableCell proto_cell;
    proto_cell.set_row_span(cell.row_span);
    proto_cell.set_col_span(cell.col_span);
    proto_cell.set_start_row_offset_idx(cell.row);
    proto_cell.set_end_row_offset_idx(cell.row + cell.row_span);
    proto_cell.set_start_col_offset_idx(cell.col);
    proto_cell.set_end_col_offset_idx(cell.col + cell.col_span);
    proto_cell.set_column_header(cell.header);
    std::string text;
    for (size_t member_index = 0; member_index < lines.size(); ++member_index) {
      if (line_taken[member_index]) continue;
      const auto& member = lines[member_index];
      const cv::Point center = member.box.center();
      const bool contains = center.x >= cell.left && center.x <= cell.right &&
                            center.y >= cell.top && center.y <= cell.bottom;
      if (!contains) continue;
      line_taken[member_index] = true;
      if (!text.empty()) text.push_back(' ');
      text += page.lines[member.index].text;
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
  if (!region.structured_cells.empty()) {
    fill_structured_table_data(page, region, data);
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
