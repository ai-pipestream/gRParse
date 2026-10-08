// Calamine is the primary reader of workbook cells. A routed workbook runs
// libreoffice first and calamine beside it: libreoffice gives the document
// its shape (sheet groups, charts, pictures, provenance, metadata) and
// calamine gives every sheet table its cells, because calamine reads the
// stored values (a text cell stays text, a number stays the number written)
// where libreoffice's import re-parses them.
//
// adopt_calamine_cells swaps the cells of each libreoffice sheet table for
// calamine's reading of the same sheet, keeps the merged areas libreoffice
// saw where calamine reported none (ODS, XLSB), and marks the header band
// again over the cells that stayed. The libreoffice TableItem itself stays,
// so every ref into it (a chart's bound table, a picture anchored on the
// sheet) still resolves.
#pragma once

#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

// One merged area of a sheet, in absolute zero-based cell coordinates.
struct MergedArea {
  int row = 0;
  int column = 0;
  int rows = 1;
  int columns = 1;
};

// Gives the top-left cell of every merged area the area's spans and drops
// the other cells inside it. An area whose top-left cell holds nothing
// stays unplaced: a blank merge is layout, not content.
void apply_merged_areas(ai::pipestream::document::v1::TableData* data,
                        const std::vector<MergedArea>& areas);

// Marks a sheet table's section row and header band (the labels-over-data
// rule, office_fold/sheet_header_band.h) after clearing any earlier marks.
// Returns the number of cells marked column_header.
int mark_sheet_header(ai::pipestream::document::v1::TableData* data);

// Moves calamine's cells into the matching sheet tables of `merged` (matched
// by sheet name). Sheets calamine read that `merged` has no table for are
// left out and named in `warnings`. Returns the number of sheet tables
// whose cells came from calamine.
int adopt_calamine_cells(const ai::pipestream::document::v1::Document& calamine,
                         ai::pipestream::document::v1::Document* merged,
                         std::vector<std::string>* warnings);

}  // namespace grparse
