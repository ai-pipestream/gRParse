// The spreadsheet plane: one group and one absolute-address table per
// sheet, its rows, its named and database ranges, its cell notes and its
// pivot definitions. It also answers the questions a chart on a sheet asks
// about the sheet it sits on.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "grparse/office_fold/arena.h"
#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

class ChartFold;

class SheetFold {
 public:
  explicit SheetFold(DocumentArena& arena) : arena_(arena) {}

  void on_sheet(const officev1::Sheet& sheet);
  void on_sheet_row(const officev1::SheetRow& row);
  void on_named_range(const officev1::SheetNamedRange& range);
  void on_database_range(const officev1::SheetDatabaseRange& range);
  void on_cell_comment(const officev1::SheetCellComment& comment);
  // The sheet's draw page announced the chart as an embedded object before
  // the sheet streamed; that event carries the series, this one the source
  // ranges. Together they are one chart under the sheet.
  void on_chart(const officev1::SheetChart& chart, ChartFold& charts);
  void on_pivot_table(const officev1::SheetPivotTable& pivot);

  // The name of a sheet by its zero-based ordinal; empty when no Sheet
  // header for it has arrived.
  std::string label(int index) const;
  // True once a Sheet header has mapped this ordinal.
  bool has_sheet(int index) const { return sheet_group_.contains(index); }
  // The sheet's group, or the body when no Sheet header for it arrived.
  std::string group_ref(int index) const;
  // The sheet's content layer; hidden sheets map to the invisible layer.
  docv1::ContentLayer layer(int index) const;
  // The sheet's folded cell table; null when the sheet was never mapped.
  const docv1::TableData* sheet_data(int index) const;

  // A sheet that streamed no cell is empty: its table says 0 x 0 rather
  // than the 1 x 1 its used range reads as (used_end_row and
  // used_end_column are zero for an empty sheet and for a sheet whose only
  // cell is A1 alike, and only the cells tell the two apart).
  void size_empty_tables();
  // Marks the header row of each sheet table once every row has arrived:
  // a database range that declares one, else the first row with two or
  // more text cells directly above a row carrying typed quantities. A lone
  // merged text cell spanning the width above the header is a section row.
  void mark_header_rows();
  // Names the sheet of every range that was declared before its sheet
  // header arrived.
  void resolve_named_range_sheets();

 private:
  // The cells of one table by row, in arrival order (ascending rows,
  // ascending columns).
  using RowCells = std::map<int, std::vector<docv1::TableCell*>>;

  // Marks the header cells every database range on the sheet declares.
  // True when the sheet declared one at all, which rules the inference out.
  bool mark_declared_headers(int sheet_index, const RowCells& rows,
                             int* marked);
  // The first row of labels directly above a row of quantities, when no
  // range declared a header row.
  void mark_inferred_header(docv1::TableData* data, const RowCells& rows,
                            int* marked);

  DocumentArena& arena_;
  // Per-sheet arena bookkeeping: the sheet's group ref, its folded table's
  // arena index, its lazily created comment-section group ref, its name and
  // its content layer.
  std::map<int, std::string> sheet_group_;
  std::map<int, int> sheet_table_;
  std::map<int, std::string> sheet_comments_;
  std::map<int, std::string> sheet_name_;
  std::map<int, docv1::ContentLayer> sheet_layer_;
  // Named ranges arrive before the sheets they sit on, so the sheet each
  // one names is filled in once the sheet headers have streamed past:
  // (index in Document.named_ranges, zero-based sheet ordinal).
  std::vector<std::pair<int, int>> pending_range_sheets_;
};

}  // namespace grparse::office_fold
