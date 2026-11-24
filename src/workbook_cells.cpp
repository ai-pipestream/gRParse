#include "grparse/workbook_cells.h"

#include <algorithm>
#include <cmath>
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

// The cell's sheet address: its grid position plus its table's origin.
Position address_of(const docv1::TableCell& cell, const office_fold::GridOrigin& origin) {
  return {cell.start_row_offset_idx() + origin.row,
          cell.start_col_offset_idx() + origin.column};
}

// Moves a cell from its place in a cut table to its sheet address.
void move_to_address(docv1::TableCell* cell, const office_fold::GridOrigin& origin) {
  cell->set_start_row_offset_idx(cell->start_row_offset_idx() + origin.row);
  cell->set_end_row_offset_idx(cell->end_row_offset_idx() + origin.row);
  cell->set_start_col_offset_idx(cell->start_col_offset_idx() + origin.column);
  cell->set_end_col_offset_idx(cell->end_col_offset_idx() + origin.column);
}

void set_spans(docv1::TableCell* cell, int rows, int columns) {
  cell->set_row_span(rows);
  cell->set_col_span(columns);
  cell->set_end_row_offset_idx(cell->start_row_offset_idx() + rows);
  cell->set_end_col_offset_idx(cell->start_col_offset_idx() + columns);
}

// The direct table of every SHEET group under the body, by sheet name. A
// sheet group holds exactly one table; a second one (never produced by
// either fold) is left alone rather than guessed at. A sheet group deeper
// down is a sheet of a workbook embedded in the document, which can share a
// name with a sheet of the document and is none of them.
std::map<std::string, int> sheet_tables(const docv1::Document& document) {
  std::map<std::string, int> tables;
  std::map<std::string_view, int> by_ref;
  for (int index = 0; index < document.tables_size(); ++index) {
    by_ref.emplace(document.tables(index).self_ref(), index);
  }
  for (const docv1::GroupItem& group : document.groups()) {
    if (group.label() != docv1::GROUP_LABEL_SHEET) continue;
    if (group.parent().ref() != "#/body") continue;
    for (const docv1::RefItem& child : group.children()) {
      const auto found = by_ref.find(child.ref());
      if (found == by_ref.end()) continue;
      tables.emplace(group.name(), found->second);
      break;
    }
  }
  return tables;
}

// True when calamine's value names the number libreoffice read. calamine
// states an integer cell as an integer and libreoffice states every number
// as a double, so an integer agrees with the double that holds exactly it.
// The comparison is made in integers: an integer a double cannot hold (past
// 2^53) is not the rounded double beside it.
bool states_number(const docv1::CellValue& calamine, double number) {
  switch (calamine.kind_case()) {
    case docv1::CellValue::kNumber:
      return calamine.number() == number;
    case docv1::CellValue::kInteger: {
      // Doubles from -2^63 up to, not including, 2^63 convert to int64
      // without overflow; a double with a fraction equals no integer.
      constexpr double kLimit = 9223372036854775808.0;
      if (!(number >= -kLimit && number < kLimit) || number != std::trunc(number)) return false;
      return static_cast<int64_t>(number) == calamine.integer() &&
             static_cast<double>(calamine.integer()) == number;
    }
    default:
      return false;
  }
}

// One cell of the adopted table: calamine's text and value over the
// libreoffice cell at the same place when there is one, which keeps that
// cell's presentation (alignment, a cell's anchored ref, a number format,
// a formula's source). Inline runs index into libreoffice's text, so they
// survive only when the two texts agree. The one cell calamine does not
// override is a formula it read no result for.
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
  // Preserve a source display string only when both readers agree on the
  // numeric value. An imported date or a genuinely different number must
  // still yield to calamine's stored value.
  const bool same_formatted_number = libre->has_value() && calamine.has_value()
      && libre->value().has_number_format()
      && libre->value().kind_case() == docv1::CellValue::kNumber
      && states_number(calamine.value(), libre->value().number());
  // A formula calamine has no result for is one the file stores with no
  // cached value. That is not a stored value to prefer over libreoffice's:
  // libreoffice computed the result when it loaded the file, and an empty
  // text here would erase it.
  const bool uncached_formula = calamine.text().empty() && calamine.has_value()
      && calamine.value().kind_case() == docv1::CellValue::KIND_NOT_SET
      && calamine.value().has_formula_source();
  if (!same_formatted_number && !uncached_formula) {
    if (cell.text() != calamine.text()) cell.clear_spans();
    cell.set_text(calamine.text());
  }
  std::optional<std::string> number_format;
  std::optional<std::string> formula;
  if (libre->has_value()) {
    if (libre->value().has_number_format()) number_format = libre->value().number_format();
    if (libre->value().has_formula_source()) formula = libre->value().formula_source();
    else if (libre->value().kind_case() == docv1::CellValue::kFormula) {
      formula = libre->value().formula();
    }
  }
  // The computed result stays in `kind`; calamine's account of the cell (its
  // source, its style) joins it.
  if (uncached_formula) cell.mutable_value()->MergeFrom(calamine.value());
  else if (calamine.has_value()) *cell.mutable_value() = calamine.value();
  else cell.clear_value();
  if (number_format && !cell.value().has_number_format()) {
    cell.mutable_value()->set_number_format(*number_format);
  }
  // Keep the primary's formula source, but retain calamine's cached value.
  // A calamine-only cell already carries its own formula_source.
  if (formula) cell.mutable_value()->set_formula_source(*formula);
  return cell;
}

// Replaces the cells of `libre` with calamine's reading of the same sheet,
// keeping libreoffice's merged areas where calamine reported none. Each
// table is cut to its own used range, so the two are laid over each other
// at their sheet addresses, and the result is cut to the cells it holds.
void adopt_table(const docv1::TableData& calamine, docv1::TableData* libre) {
  const office_fold::GridOrigin calamine_origin = office_fold::origin_of(calamine);
  const office_fold::GridOrigin libre_origin = office_fold::origin_of(*libre);
  std::map<Position, const docv1::TableCell*> libre_cells;
  std::vector<MergedArea> libre_merges;
  for (const docv1::TableCell& cell : libre->table_cells()) {
    const Position address = address_of(cell, libre_origin);
    libre_cells.emplace(address, &cell);
    if (cell.row_span() > 1 || cell.col_span() > 1) {
      libre_merges.push_back({address.first, address.second, std::max(1, cell.row_span()),
                              std::max(1, cell.col_span())});
    }
  }
  const bool calamine_merges = std::ranges::any_of(
      calamine.table_cells(),
      [](const docv1::TableCell& cell) { return cell.row_span() > 1 || cell.col_span() > 1; });

  // The adopted cells, at their sheet addresses.
  docv1::TableData adopted;
  for (const docv1::TableCell& cell : calamine.table_cells()) {
    const auto found = libre_cells.find(address_of(cell, calamine_origin));
    docv1::TableCell* placed = adopted.add_table_cells();
    *placed = adopted_cell(cell, found == libre_cells.end() ? nullptr : found->second);
    move_to_address(placed, calamine_origin);
  }
  // A libreoffice cell with no text of its own still matters when it anchors
  // something (a picture or a control placed in the cell).
  std::set<Position> placed;
  for (const docv1::TableCell& cell : adopted.table_cells()) placed.insert(position_of(cell));
  for (const docv1::TableCell& cell : libre->table_cells()) {
    if (cell.has_ref() && !placed.contains(address_of(cell, libre_origin))) {
      docv1::TableCell* kept = adopted.add_table_cells();
      *kept = cell;
      move_to_address(kept, libre_origin);
      kept->set_column_header(false);
      kept->set_row_header(false);
      kept->set_row_section(false);
    }
  }
  // calamine's spans came with its cells; libreoffice's fill in only for a
  // format calamine reads no merges from (ODS, XLSB).
  if (!calamine_merges) apply_merged_areas(&adopted, libre_merges);

  // The sheet's extent, in sheet rows and columns.
  int rows = calamine.num_rows() + calamine_origin.row;
  int columns = calamine.num_cols() + calamine_origin.column;
  for (const docv1::TableCell& cell : adopted.table_cells()) {
    rows = std::max(rows, cell.end_row_offset_idx());
    columns = std::max(columns, cell.end_col_offset_idx());
  }
  libre->mutable_table_cells()->Swap(adopted.mutable_table_cells());
  libre->set_num_rows(rows);
  libre->set_num_cols(columns);
  libre->clear_grid();
  libre->clear_origin();
  *libre->mutable_declared_tables() = calamine.declared_tables();
  // A column schema describes libreoffice's columns, from its origin column
  // on; it no longer fits when calamine's sheet is a different width.
  if (libre->columns_size() + libre_origin.column != columns) libre->clear_columns();
  office_fold::cut_to_used_range(libre, libre_origin.column);
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
  if (!data->declared_tables().empty()) {
    // A declared range is in sheet addresses; the cells are the cut table's.
    const office_fold::GridOrigin origin = office_fold::origin_of(*data);
    int marked = 0;
    for (auto& cell : *data->mutable_table_cells()) {
      const auto [row, column] = address_of(cell, origin);
      cell.set_column_header(false);
      cell.set_row_section(false);
      for (const auto& table : data->declared_tables()) {
        const auto& first = table.range().start();
        const auto& last = table.range().end();
        const int64_t header_end = std::min<int64_t>(
            static_cast<int64_t>(last.row()) + 1,
            static_cast<int64_t>(first.row()) + table.header_row_count());
        if (row >= first.row() && row < header_end && column >= first.col()
            && column <= last.col()) {
          cell.set_column_header(true);
          ++marked;
          break;
        }
      }
    }
    // A sheet can contain several separately named tables. Only a single
    // width-spanning declaration describes the sheet table's whole schema.
    if (data->declared_tables_size() == 1) {
      const auto& table = data->declared_tables(0);
      if (table.range().start().col() == origin.column
          && static_cast<int64_t>(table.range().end().col()) + 1
                 == static_cast<int64_t>(origin.column) + data->num_cols()
          && table.columns_size() == data->num_cols()) {
        while (data->columns_size() < data->num_cols()) data->add_columns();
        for (int i = 0; i < table.columns_size(); ++i) {
          data->mutable_columns(i)->set_name(table.columns(i));
        }
      }
    }
    return marked;
  }
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

int adopt_calamine_cells(const docv1::Document& calamine,
                         const std::vector<IncompleteSheet>& incomplete,
                         docv1::Document* merged, std::vector<Warning>* warnings) {
  const std::map<std::string, int> targets = sheet_tables(*merged);
  // A sheet calamine did not read to its end keeps the cells it has: the
  // part calamine holds (which may be nothing) would replace a whole sheet
  // with fewer cells. The warning names the table, so a consumer knows by
  // code which sheets are the first reader's and which are calamine's.
  std::set<std::string_view> held_back;
  for (const IncompleteSheet& sheet : incomplete) {
    held_back.insert(sheet.name);
    const auto target = targets.find(sheet.name);
    if (target == targets.end()) continue;
    const docv1::TableItem& table = merged->tables(target->second);
    const std::string reader =
        table.source_size() > 0 && table.source(0).has_collector()
            ? table.source(0).collector().collector()
            : "the first reader";
    // A status can fail with no words of its own; the sentence stands
    // without them.
    const std::string why = sheet.reason.empty() ? "" : " (" + sheet.reason + ")";
    warnings->push_back(
        Warning(docv1::WARNING_CODE_SHEET_CELLS_NOT_ADOPTED,
                "sheet '" + sheet.name + "': calamine did not read it to the end" + why
                    + ", so its cells were not adopted; the sheet keeps " + reader
                    + "'s cells")
            .on_item(table.self_ref()));
  }
  int adopted = 0;
  for (const auto& [sheet, calamine_index] : sheet_tables(calamine)) {
    const auto target = targets.find(sheet);
    if (target == targets.end()) {
      warnings->push_back("sheet '" + sheet
                          + "': calamine read it but the document has no sheet table "
                            "for it, so its cells were left out");
      continue;
    }
    if (held_back.contains(sheet)) continue;
    docv1::TableItem* table = merged->mutable_tables(target->second);
    // Kept by sheet address: the adoption can move the table's origin.
    std::set<Position> libre_header;
    const office_fold::GridOrigin before = office_fold::origin_of(table->data());
    for (const docv1::TableCell& cell : table->data().table_cells()) {
      if (cell.column_header()) libre_header.insert(address_of(cell, before));
    }
    adopt_table(calamine.tables(calamine_index).data(), table->mutable_data());
    // The calamine fold already named the inference when it marked a band
    // over these cells, so the mark is not reported a second time here.
    // When the band finds nothing, libreoffice's header stands: a database
    // range can declare a header the rule would not infer.
    if (mark_sheet_header(table->mutable_data()) == 0
        && table->data().declared_tables().empty()) {
      const office_fold::GridOrigin after = office_fold::origin_of(table->data());
      for (docv1::TableCell& cell : *table->mutable_data()->mutable_table_cells()) {
        if (libre_header.contains(address_of(cell, after))) cell.set_column_header(true);
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
