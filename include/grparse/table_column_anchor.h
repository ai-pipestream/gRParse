#pragma once

#include <vector>

#include "grparse/ocr_types.h"
#include "grparse/text_geometry.h"

namespace grparse {

// Moves body words the structure model put in the wrong column. On a dense
// table the model's cell boxes drift from the words by up to a column, so
// the same visual column lands in one model column on some rows and the
// next on others. The words' own gutters give the visual columns (bands);
// a band whose rows the model put in more than one column goes to the
// column whose heading words sit over it, when that is one of them, else
// to the column most of its rows went to. A band the model kept in one
// column is never touched, nor are heading rows, spanning cells or tables
// that are not mostly numbers. `cell_of_line` is assign_table_lines' map.
void anchor_body_columns(const OcrPage& page, const LayoutRegion& table, std::vector<int>& cell_of_line);

}  // namespace grparse
