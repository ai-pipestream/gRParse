#include "grparse/office_fold/sheet_fold.h"

#include <algorithm>
#include <iterator>
#include <set>

#include "grparse/data_totals.h"
#include "grparse/office_fold/chart_fold.h"
#include "grparse/office_fold/grid_cells.h"
#include "grparse/office_fold/value_convert.h"

namespace grparse::office_fold {

namespace {

// The cell's typed value: text stays the display string, and the value says
// what the sheet actually holds. An error beats a formula, because a
// formula that failed has no value to report. False when the cell holds
// nothing the schema types.
bool typed_cell_value(const officev1::SheetCell& cell, docv1::CellValue* out) {
  bool typed = true;
  if (cell.error_code() != 0) {
    out->set_error(cell.display().empty()
                       ? "Err:" + std::to_string(cell.error_code())
                       : cell.display());
  } else if (cell.type() == officev1::SHEET_CELL_TYPE_FORMULA) {
    out->set_formula(cell.formula());
  } else if (cell.is_boolean()) {
    out->set_boolean(cell.number() != 0);
  } else if (cell.is_datetime()) {
    // A spreadsheet date is a wall-clock value; it stays one.
    docv1::CivilDateTime* when = out->mutable_datetime();
    when->set_year(cell.datetime().year());
    when->set_month(cell.datetime().month());
    when->set_day(cell.datetime().day());
    when->set_hour(cell.datetime().hour());
    when->set_minute(cell.datetime().minute());
    when->set_second(cell.datetime().second());
  } else if (cell.type() == officev1::SHEET_CELL_TYPE_VALUE) {
    out->set_number(cell.number());
  } else {
    typed = false;
  }
  if (!cell.number_format_string().empty()) {
    out->set_number_format(cell.number_format_string());
    typed = true;
  }
  return typed;
}

// One placed cell of a sheet row, in the sheet's absolute addresses.
docv1::TableCell* place_sheet_cell(const officev1::SheetCell& cell, int row,
                                   docv1::TableData* data) {
  docv1::TableCell* out = data->add_table_cells();
  out->set_start_row_offset_idx(row);
  out->set_end_row_offset_idx(row + std::max(1, cell.merged_rows()));
  out->set_start_col_offset_idx(cell.column());
  out->set_end_col_offset_idx(cell.column()
                              + std::max(1, cell.merged_columns()));
  out->set_row_span(std::max(1, cell.merged_rows()));
  out->set_col_span(std::max(1, cell.merged_columns()));
  out->set_text(cell.display());
  return out;
}

}  // namespace

std::string SheetFold::label(int index) const {
  auto found = sheet_name_.find(index);
  return found != sheet_name_.end() ? found->second : std::string();
}

std::string SheetFold::group_ref(int index) const {
  auto found = sheet_group_.find(index);
  return found != sheet_group_.end() ? found->second : std::string("#/body");
}

docv1::ContentLayer SheetFold::layer(int index) const {
  auto found = sheet_layer_.find(index);
  return found != sheet_layer_.end() ? found->second
                                     : docv1::CONTENT_LAYER_BODY;
}

const docv1::TableData* SheetFold::sheet_data(int index) const {
  auto found = sheet_table_.find(index);
  if (found == sheet_table_.end()) return nullptr;
  return &arena_.document().tables(found->second).data();
}

void SheetFold::on_sheet(const officev1::Sheet& sheet) {
  docv1::ContentLayer layer = sheet.visible()
      ? docv1::CONTENT_LAYER_BODY
      : docv1::CONTENT_LAYER_INVISIBLE;
  sheet_layer_[sheet.index()] = layer;
  docv1::GroupItem* group = arena_.add_group("#/body", docv1::GROUP_LABEL_SHEET,
                                             sheet.name(), layer);
  docv1::SheetMeta* attributes = group->mutable_sheet();
  attributes->set_index(sheet.index());
  attributes->set_visible(sheet.visible());
  if (sheet.tab_color_rgb() >= 0) {
    attributes->set_tab_color(
        hex_color(static_cast<uint32_t>(sheet.tab_color_rgb())));
  }
  for (const officev1::SheetRangeRef& area : sheet.print_areas()) {
    set_grid_span(area, sheet.name(), attributes->add_print_areas());
  }
  sheet_group_[sheet.index()] = group->self_ref();
  sheet_name_[sheet.index()] = sheet.name();

  // The sheet's cell grid folds into one TableItem in absolute row and
  // column offsets, so cell addresses survive the mapping.
  sheet_table_[sheet.index()] = arena_.document().tables_size();
  docv1::TableItem* table = arena_.add_table(layer, group->self_ref(), nullptr);
  docv1::TableData* data = table->mutable_data();
  data->set_num_rows(sheet.used_end_row() + 1);
  data->set_num_cols(sheet.used_end_column() + 1);
  // The sheet's columns are its schema: each one names its spreadsheet
  // column and carries the declared width in the page unit.
  for (int column = 0; column < sheet.column_widths_twips_size(); column++) {
    docv1::TableColumnSchema* schema = data->add_columns();
    schema->set_name(column_name(column));
    schema->set_width(static_cast<double>(sheet.column_widths_twips(column)));
  }
  arena_.add_prov(table->mutable_prov(), sheet.index(), true, 0, 0, 0, 0, 0, 0);
  if (table->prov_size() > 0 && !sheet.name().empty()) {
    table->mutable_prov(0)->mutable_grid()->set_sheet(sheet.name());
  }
}

void SheetFold::on_sheet_row(const officev1::SheetRow& row) {
  auto found = sheet_table_.find(row.sheet_index());
  if (found == sheet_table_.end()) return;
  docv1::TableData* data =
      arena_.document().mutable_tables(found->second)->mutable_data();
  // One provenance entry per used row, locating it in the sheet grid; the
  // cells themselves carry no provenance slot.
  if (!row.cells().empty()) {
    docv1::ProvenanceItem* row_prov = data->add_row_prov();
    row_prov->set_page_no(row.sheet_index() + 1);
    docv1::GridCell* grid = row_prov->mutable_grid();
    grid->set_row(row.row());
    grid->set_col(row.cells(0).column());
    if (auto name = sheet_name_.find(row.sheet_index());
        name != sheet_name_.end()) {
      grid->set_sheet(name->second);
    }
  }
  for (const officev1::SheetCell& cell : row.cells()) {
    docv1::TableCell* out = place_sheet_cell(cell, row.row(), data);
    docv1::CellValue value;
    if (typed_cell_value(cell, &value)) *out->mutable_value() = value;
  }
}

void SheetFold::on_named_range(const officev1::SheetNamedRange& range) {
  docv1::NamedRange* out = arena_.document().add_named_ranges();
  out->set_name(range.name());
  out->set_kind("named");
  // A name pointing at cells resolves to a span; a name holding an
  // expression has no rectangle and keeps only its name. Workbook-scoped
  // names arrive before any sheet header, so the sheet is named later.
  if (range.has_range()) {
    set_grid_span(range.range(), std::string(), out->mutable_range());
    if (range.sheet_index() >= 0) {
      pending_range_sheets_.emplace_back(
          arena_.document().named_ranges_size() - 1, range.sheet_index());
    }
  } else if (!range.content().empty()) {
    out->set_expression(range.content());
  }
}

void SheetFold::on_database_range(const officev1::SheetDatabaseRange& range) {
  docv1::NamedRange* out = arena_.document().add_named_ranges();
  out->set_name(range.name());
  out->set_kind("database");
  set_grid_span(range.range(), std::string(), out->mutable_range());
  if (range.sheet_index() >= 0) {
    pending_range_sheets_.emplace_back(
        arena_.document().named_ranges_size() - 1, range.sheet_index());
  }
  out->set_has_headers(range.contains_header());
  out->set_has_totals(range.totals_row());
}

void SheetFold::on_cell_comment(const officev1::SheetCellComment& comment) {
  const std::string sheet_ref = group_ref(comment.sheet_index());
  const docv1::ContentLayer content_layer = layer(comment.sheet_index());
  auto comments = sheet_comments_.find(comment.sheet_index());
  if (comments == sheet_comments_.end()) {
    docv1::GroupItem* section = arena_.add_group(
        sheet_ref, docv1::GROUP_LABEL_COMMENT_SECTION, "comments",
        content_layer);
    comments = sheet_comments_
        .emplace(comment.sheet_index(), section->self_ref()).first;
  }
  TextHandle handle = arena_.add_text(TextKind::kText,
                                      docv1::DOC_ITEM_LABEL_TEXT,
                                      content_layer, comments->second);
  handle.base->set_text(comment.text());
  handle.base->set_orig(comment.text());
  docv1::CommentMeta* identity = handle.base->mutable_comment_meta();
  if (!comment.author().empty()) identity->set_author(comment.author());
  // The office core hands a sheet annotation's date over as its own string
  // and does not say in what format, so it stays the raw spelling.
  if (!comment.date().empty()) identity->set_timestamp_raw(comment.date());
  // The cell a note is attached to is a position in the sheet grid.
  docv1::ProvenanceItem* where = handle.base->add_prov();
  where->set_page_no(comment.sheet_index() + 1);
  docv1::GridCell* grid = where->mutable_grid();
  grid->set_row(comment.row());
  grid->set_col(comment.column());
  const std::string sheet = label(comment.sheet_index());
  if (!sheet.empty()) grid->set_sheet(sheet);
  identity->set_shown(comment.visible());
  auto table = sheet_table_.find(comment.sheet_index());
  if (table != sheet_table_.end()) {
    arena_.document().mutable_tables(table->second)->add_comments()->set_ref(
        handle.ref);
  }
}

void SheetFold::on_chart(const officev1::SheetChart& chart, ChartFold& charts) {
  const std::string sheet_ref = group_ref(chart.sheet_index());
  const docv1::ContentLayer content_layer = layer(chart.sheet_index());
  officev1::EmbeddedObject object;
  if (charts.take_pending(chart.sheet_index(), nullptr, &object)) {
    const double l = static_cast<double>(object.position().x());
    const double t = static_cast<double>(object.position().y());
    charts.emit(&object, &chart, sheet_ref, content_layer, true,
                chart.sheet_index(), l, t,
                l + static_cast<double>(object.width_twips()),
                t + static_cast<double>(object.height_twips()));
    return;
  }
  charts.emit(nullptr, &chart, sheet_ref, content_layer, true,
              chart.sheet_index(), 0, 0, 0, 0);
}

void SheetFold::on_pivot_table(const officev1::SheetPivotTable& pivot) {
  const std::string sheet_ref = group_ref(pivot.sheet_index());
  docv1::TableItem* table =
      arena_.add_table(layer(pivot.sheet_index()), sheet_ref, nullptr);
  const officev1::SheetRangeRef& output = pivot.output_range();
  table->mutable_data()->set_num_rows(output.end_row() - output.start_row()
                                      + 1);
  table->mutable_data()->set_num_cols(output.end_column()
                                      - output.start_column() + 1);
  // The definition is a declaration of the workbook, not of the output
  // table, so it lives beside the document with its ranges as grid spans.
  const std::string sheet = label(pivot.sheet_index());
  docv1::PivotSpec* spec = arena_.document().add_pivots();
  spec->set_name(pivot.name());
  set_grid_span(pivot.source_range(), sheet, spec->mutable_source());
  set_grid_span(output, sheet, spec->mutable_output());
  for (const std::string& name : pivot.row_fields()) spec->add_row_fields(name);
  for (const std::string& name : pivot.column_fields()) {
    spec->add_column_fields(name);
  }
  for (const std::string& name : pivot.data_fields()) {
    spec->add_data_fields(name);
  }
  for (const std::string& name : pivot.page_fields()) {
    spec->add_page_fields(name);
  }
  arena_.add_prov(table->mutable_prov(), pivot.sheet_index(), true, 0, 0, 0, 0,
                  0, 0);
}

void SheetFold::size_empty_tables() {
  for (const auto& [sheet_index, table_index] : sheet_table_) {
    docv1::TableData* data =
        arena_.document().mutable_tables(table_index)->mutable_data();
    if (data->table_cells_size() > 0) continue;
    data->set_num_rows(0);
    data->set_num_cols(0);
  }
}

bool SheetFold::mark_declared_headers(int sheet_index, const RowCells& rows,
                                      int* marked) {
  // A database range that declares a header names the row outright. The
  // range's sheet is still pending here (names resolve with the anchors),
  // so the pending list is what pairs a range with its sheet.
  bool declared = false;
  const docv1::Document& document = arena_.document();
  for (const auto& [range_index, range_sheet] : pending_range_sheets_) {
    if (range_sheet != sheet_index
        || range_index >= document.named_ranges_size()) {
      continue;
    }
    const docv1::NamedRange& range = document.named_ranges(range_index);
    if (range.kind() != "database" || !range.has_headers()
        || !range.has_range()) {
      continue;
    }
    declared = true;
    auto found = rows.find(range.range().start().row());
    if (found == rows.end()) continue;
    for (docv1::TableCell* cell : found->second) {
      const int column = cell->start_col_offset_idx();
      if (column < range.range().start().col()
          || column > range.range().end().col()) {
        continue;
      }
      if (!cell->column_header()) (*marked)++;
      cell->set_column_header(true);
    }
  }
  return declared;
}

void SheetFold::mark_inferred_header(docv1::TableData* data,
                                     const RowCells& rows, int* marked) {
  // The first row with two or more labels directly above a row that carries
  // quantities; a lone merged label spanning the used width above it is a
  // section row, not a header.
  auto it = rows.begin();
  if (it->second.size() == 1 && it->second[0]->col_span() >= 2
      && it->second[0]->col_span() >= data->num_cols()
      && label_cell(*it->second[0])) {
    it->second[0]->set_row_section(true);
    ++it;
  }
  if (it == rows.end()) return;
  auto next = std::next(it);
  const bool labels =
      it->second.size() >= 2
      && std::ranges::all_of(it->second, [](const docv1::TableCell* cell) {
           return label_cell(*cell);
         });
  const bool quantities_below =
      next != rows.end()
      && std::ranges::any_of(next->second, [](const docv1::TableCell* cell) {
           return quantity_cell(*cell);
         });
  if (!labels || !quantities_below) return;
  for (docv1::TableCell* cell : it->second) cell->set_column_header(true);
  *marked += static_cast<int>(it->second.size());
}

void SheetFold::mark_header_rows() {
  for (const auto& [sheet_index, table_index] : sheet_table_) {
    docv1::TableData* data =
        arena_.document().mutable_tables(table_index)->mutable_data();
    RowCells rows;
    for (docv1::TableCell& cell : *data->mutable_table_cells()) {
      rows[cell.start_row_offset_idx()].push_back(&cell);
    }
    if (rows.empty()) continue;
    int marked = 0;
    if (!mark_declared_headers(sheet_index, rows, &marked)) {
      mark_inferred_header(data, rows, &marked);
    }
    if (marked > 0) {
      data_counters().sheet_header_rows.fetch_add(1, std::memory_order_relaxed);
      data_log("sheet " + label(sheet_index) + ": header row marked ("
               + std::to_string(marked) + " cells)");
    }
  }
}

void SheetFold::resolve_named_range_sheets() {
  for (const auto& [range_index, sheet_index] : pending_range_sheets_) {
    const std::string sheet = label(sheet_index);
    if (sheet.empty()
        || range_index >= arena_.document().named_ranges_size()) {
      continue;
    }
    docv1::GridSpan* span =
        arena_.document().mutable_named_ranges(range_index)->mutable_range();
    span->mutable_start()->set_sheet(sheet);
    span->mutable_end()->set_sheet(sheet);
  }
}

}  // namespace grparse::office_fold
