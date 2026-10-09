// Header inference for a sheet table that declares no header: the band of
// label rows that sits over the first data region. Pure functions over row
// summaries, so the rule can be tested without a document.
//
// Report-style sheets carry a preamble (a title, a "RUN AT" line, notes,
// blank rows) before the header, and the header itself can be two or three
// lines (a group line over a leaf line). The rule:
//
//  1. The data starts at the first row that carries a quantity and is at
//     least half as wide as the widest quantity-bearing row (a lone date in
//     a title line is preamble, not data).
//  2. Walking up from there over rows that are not wide label rows (blank,
//     narrow, section labels, two-line records), the nearest contiguous run
//     of wide label rows is the candidate band. A wide label row holds two
//     or more labels, no quantity, and covers at least half the data width.
//  3. Header lines get denser going down: a row of the run joins the band
//     under the row above it only when it holds more labels, or when the
//     row above is a group line (a merged label). A record that merely
//     lacks its number does neither, so it stays data.
//  4. The band's densest line names more columns than the first data row
//     fills with labels: a header names the quantity columns, a record
//     that lacks its numbers does not.
//  5. A sheet with no quantity at all has a header only when its first row
//     is a full-width label row over at least two consecutive label rows:
//     without numbers nothing else tells a header from a record.
//
// A wrong header is worse than none, so every step prefers to mark nothing.
#pragma once

#include <vector>

#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

// One used row of a sheet table, reduced to what the rule reads.
struct SheetRowShape {
  int index = 0;       // the sheet row
  int labels = 0;      // cells that read as a label (text, no typed value)
  int quantities = 0;  // cells that read as a quantity
  int coverage = 0;    // columns covered by non-blank cells (spans counted)
  bool group_line = false;  // holds a label merged over two or more columns

  // Two or more labels, no quantity, at least half the data width.
  bool wide_labels(int width) const {
    return labels >= 2 && quantities == 0 && coverage * 2 >= width;
  }
};

// A band never runs deeper than this many lines.
inline constexpr int kMaxHeaderBandRows = 4;

// The shape of one row from its placed cells (any order).
SheetRowShape sheet_row_shape(int index,
                              const std::vector<docv1::TableCell*>& cells);

// The sheet rows that form the header band, ascending; empty when the rule
// finds none. `rows` are the used rows in ascending order, blank rows
// absent, with any leading section row already removed.
std::vector<int> header_band_rows(const std::vector<SheetRowShape>& rows);

}  // namespace grparse::office_fold
