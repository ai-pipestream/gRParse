// The grid vocabulary shared by every tabular fold: cell names, spans, the
// reading of a cell as a label or a quantity, and the materialization of a
// grid from placed cells. Pure functions over the document schema.
#pragma once

#include <string>

#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

// Grids above this cell count keep table_cells only; a fully materialized
// grid over a sparse used range would dwarf the data it carries.
inline constexpr int kMaxGridCells = 4096;

// The spreadsheet name of a zero-based column: 0 is "A", 26 is "AA".
std::string column_name(int column);

// "B7" and "B7.1.2" both anchor at row 6, column 1: an office cell name
// starts with the base-grid cell it was split from, so a split cell still
// has a place in the grid. False when the name anchors nowhere.
bool anchor_of_cell_name(const std::string& name, int* row, int* column);

// A wire cell range as a grid span, naming its sheet on both corners so a
// span stays readable without the surrounding context.
void set_grid_span(const officev1::SheetRangeRef& range,
                   const std::string& sheet, docv1::GridSpan* out);

// True when the text is a plain number as a sheet displays one: an optional
// sign, digits with optional group separators, an optional fraction.
bool numeric_display(const std::string& text);

// True when the cell's typed value is a quantity (number, date, logical)
// rather than text or an error. A formula counts by what it displays: a
// header row sits over "=ROW()-1" showing "1" exactly as over a literal 1.
bool quantity_cell(const docv1::TableCell& cell);

// True when the cell reads as a label: non-blank text with no typed value.
bool label_cell(const docv1::TableCell& cell);

// Places one cell of a grid built from typed data.
docv1::TableCell* place_cell(docv1::TableData* data, int row, int column,
                             const std::string& text);

// Materializes data->grid from table_cells for grids under kMaxGridCells:
// one slot per base position, the first cell placed at a slot keeping it.
void fill_grid_from_cells(docv1::TableData* data);

}  // namespace grparse::office_fold
