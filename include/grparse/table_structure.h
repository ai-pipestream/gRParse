#pragma once

#include <cstddef>
#include <vector>

#include "grparse/ocr_types.h"
#include "grparse/text_geometry.h"

namespace grparse {

struct TableCellGeometry {
  int row = 0;
  int col = 0;
  // Union of the member line boxes; zero for blank grid positions.
  AxisAlignedBox box;
  // Indices into page.lines in top-to-bottom, left-to-right order.  Empty for
  // blank grid positions, which still exist so the grid stays rectangular.
  std::vector<size_t> line_indices = {};
};

struct TableGrid {
  int rows = 0;
  int cols = 0;
  // Row-major rows x cols cells covering every grid position.
  std::vector<TableCellGeometry> cells = {};
};

// Geometry-only table structure (D2 v0): the lines bound to `table` are
// clustered into row bands by vertical overlap and into columns by merging
// their horizontal spans, where a horizontal gap wider than about half the
// median line height counts as a column gutter.  No model is involved, so
// spanning cells are split into their grid positions and header flags are
// never guessed; a structure model (D3) refines this later.  Deterministic
// for fixed input.  Returns an empty grid when no text binds to the table.
TableGrid build_table_grid(const OcrPage& page, const LayoutRegion& table);

}  // namespace grparse
