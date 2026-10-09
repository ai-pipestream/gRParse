#include "grparse/workbook_cells.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include "grparse/office_fold/grid_cells.h"
#include "grparse/office_fold/sheet_header_band.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

using Position = std::pair<int, int>;

Position position_of(const docv1::TableCell& cell) {
  return {cell.start_row_offset_idx(), cell.start_col_offset_idx()};
}

void set_spans(docv1::TableCell* cell, int rows, int columns) {
  cell->set_row_span(rows);
  cell->set_col_span(columns);
  cell->set_end_row_offset_idx(cell->start_row_offset_idx() + rows);
  cell->set_end_col_offset_idx(cell->start_col_offset_idx() + columns);
}

// The direct table of every SHEET group, by sheet name. A sheet group holds
// exactly one table; a second one (never produced by either fold) is left
// alone rather than guessed at.
std::map<std::string, int> sheet_tables(const docv1::Document& document) {
  std::map<std::string, int> tables;
  std::map<std::string_view, int> by_ref;
  for (int index = 0; index < document.tables_size(); ++index) {
    by_ref.emplace(document.tables(index).self_ref(), index);
  }
  for (const docv1::GroupItem& group : document.groups()) {
    if (group.label() != docv1::GROUP_LABEL_SHEET) continue;
    for (const docv1::RefItem& child : group.children()) {
      const auto found = by_ref.find(child.ref());
      if (found == by_ref.end()) continue;
      tables.emplace(group.name(), found->second);
      break;
    }
  }
  return tables;
}

// One cell of the adopted table: calamine's text and value over the
// libreoffice cell at the same place when there is one, which keeps that
// cell's presentation (alignment, a cell's anchored ref, a number format,
// a formula's source). Inline runs index into libreoffice's text, so they
// survive only when the two texts agree.
docv1::TableCell adopted_cell(const docv1::TableCell& calamine,
                              const docv1::TableCell* libre) {
  if (libre == nullptr) return calamine;
  docv1::TableCell cell = *libre;
  // Placement is calamine's: its spans are the merges it read, and a cell
  // libreoffice spanned is respanned below only for a format calamine reads
  // no merges from.
  cell.set_start_row_offset_idx(calamine.start_row_offset_idx());
  cell.set_start_col_offset_idx(calamine.start_col_offset_idx());
  set_spans(&cell, std::max(1, calamine.row_span()), std::max(1, calamine.col_span()));
  cell.set_column_header(false);
  cell.set_row_header(false);
  cell.set_row_section(false);
  if (cell.text() != calamine.text()) cell.clear_spans();
  cell.set_text(calamine.text());
  const bool formula = libre->has_value()
                       && libre->value().kind_case() == docv1::CellValue::kFormula;
  if (!formula) {
    std::optional<std::string> number_format;
    if (cell.has_value() && cell.value().has_number_format()) {
      number_format = cell.value().number_format();
    }
    if (calamine.has_value()) {
      *cell.mutable_value() = calamine.value();
      if (number_format.has_value() && !cell.value().has_number_format()) {
        cell.mutable_value()->set_number_format(*number_format);
      }
    } else {
      cell.clear_value();
    }
  }
  return cell;
}

// Replaces the cells of `libre` with calamine's reading of the same sheet,
// keeping libreoffice's merged areas where calamine reported none.
void adopt_table(const docv1::TableData& calamine, docv1::TableData* libre) {
  std::map<Position, const docv1::TableCell*> libre_cells;
  std::vector<MergedArea> libre_merges;
  for (const docv1::TableCell& cell : libre->table_cells()) {
    libre_cells.emplace(position_of(cell), &cell);
    if (cell.row_span() > 1 || cell.col_span() > 1) {
      libre_merges.push_back({cell.start_row_offset_idx(), cell.start_col_offset_idx(),
                              std::max(1, cell.row_span()), std::max(1, cell.col_span())});
    }
  }
  const bool calamine_merges = std::ranges::any_of(
      calamine.table_cells(),
      [](const docv1::TableCell& cell) { return cell.row_span() > 1 || cell.col_span() > 1; });

  docv1::TableData adopted;
  for (const docv1::TableCell& cell : calamine.table_cells()) {
    const auto found = libre_cells.find(position_of(cell));
    *adopted.add_table_cells() =
        adopted_cell(cell, found == libre_cells.end() ? nullptr : found->second);
  }
  // A libreoffice cell with no text of its own still matters when it anchors
  // something (a picture or a control placed in the cell).
  std::set<Position> placed;
  for (const docv1::TableCell& cell : adopted.table_cells()) placed.insert(position_of(cell));
  for (const docv1::TableCell& cell : libre->table_cells()) {
    if (cell.has_ref() && !placed.contains(position_of(cell))) {
      docv1::TableCell* kept = adopted.add_table_cells();
      *kept = cell;
      kept->set_column_header(false);
      kept->set_row_header(false);
      kept->set_row_section(false);
    }
  }
  // calamine's spans came with its cells; libreoffice's fill in only for a
  // format calamine reads no merges from (ODS, XLSB).
  if (!calamine_merges) apply_merged_areas(&adopted, libre_merges);

  int rows = calamine.num_rows();
  int columns = calamine.num_cols();
  for (const docv1::TableCell& cell : adopted.table_cells()) {
    rows = std::max(rows, cell.end_row_offset_idx());
    columns = std::max(columns, cell.end_col_offset_idx());
  }
  libre->mutable_table_cells()->Swap(adopted.mutable_table_cells());
  libre->set_num_rows(rows);
  libre->set_num_cols(columns);
  libre->clear_grid();
  // A column schema describes libreoffice's columns; it no longer fits when
  // calamine's sheet is a different width.
  if (libre->columns_size() != columns) libre->clear_columns();
}

}  // namespace

void apply_merged_areas(docv1::TableData* data, const std::vector<MergedArea>& areas) {
  if (areas.empty()) return;
  std::map<Position, int> index_at;
  for (int index = 0; index < data->table_cells_size(); ++index) {
    index_at.emplace(position_of(data->table_cells(index)), index);
  }
  std::set<int> covered;
  for (const MergedArea& area : areas) {
    if (area.rows < 1 || area.columns < 1 || (area.rows == 1 && area.columns == 1)) continue;
    const auto top_left = index_at.find({area.row, area.column});
    if (top_left == index_at.end()) continue;
    set_spans(data->mutable_table_cells(top_left->second), area.rows, area.columns);
    for (int row = area.row; row < area.row + area.rows; ++row) {
      for (int column = area.column; column < area.column + area.columns; ++column) {
        if (row == area.row && column == area.column) continue;
        const auto inside = index_at.find({row, column});
        if (inside != index_at.end()) covered.insert(inside->second);
      }
    }
  }
  if (covered.empty()) return;
  google::protobuf::RepeatedPtrField<docv1::TableCell> kept;
  for (int index = 0; index < data->table_cells_size(); ++index) {
    if (!covered.contains(index)) *kept.Add() = std::move(*data->mutable_table_cells(index));
  }
  data->mutable_table_cells()->Swap(&kept);
}

int mark_sheet_header(docv1::TableData* data) {
  std::map<int, std::vector<docv1::TableCell*>> rows;
  for (docv1::TableCell& cell : *data->mutable_table_cells()) {
    cell.set_column_header(false);
    cell.set_row_section(false);
    rows[cell.start_row_offset_idx()].push_back(&cell);
  }
  if (rows.empty()) return 0;
  // A lone merged label spanning the used width at the top is a section
  // row, not a header; the band rule reads the rows under it.
  auto it = rows.begin();
  if (it->second.size() == 1 && it->second[0]->col_span() >= 2
      && it->second[0]->col_span() >= data->num_cols()
      && office_fold::label_cell(*it->second[0])) {
    it->second[0]->set_row_section(true);
    ++it;
  }
  std::vector<office_fold::SheetRowShape> shapes;
  for (; it != rows.end(); ++it) {
    office_fold::SheetRowShape shape = office_fold::sheet_row_shape(it->first, it->second);
    if (shape.coverage > 0) shapes.push_back(shape);
  }
  int marked = 0;
  for (const int row : office_fold::header_band_rows(shapes)) {
    for (docv1::TableCell* cell : rows.at(row)) {
      cell->set_column_header(true);
      ++marked;
    }
  }
  return marked;
}

int adopt_calamine_cells(const docv1::Document& calamine, docv1::Document* merged,
                         std::vector<std::string>* warnings) {
  const std::map<std::string, int> targets = sheet_tables(*merged);
  int adopted = 0;
  for (const auto& [sheet, calamine_index] : sheet_tables(calamine)) {
    const auto target = targets.find(sheet);
    if (target == targets.end()) {
      warnings->push_back("sheet '" + sheet
                          + "': calamine read it but the document has no sheet table "
                            "for it, so its cells were left out");
      continue;
    }
    docv1::TableItem* table = merged->mutable_tables(target->second);
    std::set<Position> libre_header;
    for (const docv1::TableCell& cell : table->data().table_cells()) {
      if (cell.column_header()) libre_header.insert(position_of(cell));
    }
    adopt_table(calamine.tables(calamine_index).data(), table->mutable_data());
    // The calamine fold already named the inference when it marked a band
    // over these cells, so the mark is not reported a second time here.
    // When the band finds nothing, libreoffice's header stands: a database
    // range can declare a header the rule would not infer.
    if (mark_sheet_header(table->mutable_data()) == 0) {
      for (docv1::TableCell& cell : *table->mutable_data()->mutable_table_cells()) {
        if (libre_header.contains(position_of(cell))) cell.set_column_header(true);
      }
    }
    // The cells are calamine's now; the item keeps libreoffice's account of
    // where the table sits.
    docv1::SourceType source;
    source.mutable_collector()->set_collector("calamine");
    table->mutable_source()->Add(std::move(source));
    std::rotate(table->mutable_source()->rbegin(), table->mutable_source()->rbegin() + 1,
                table->mutable_source()->rend());
    ++adopted;
  }
  return adopted;
}

}  // namespace grparse
