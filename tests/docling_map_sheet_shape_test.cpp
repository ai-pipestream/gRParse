// S3 eval findings against the sheet fold: an empty or hidden sheet arrived
// as a 1 x 1 table with no cells (its used range reads 0,0 whether the sheet
// is empty or holds A1 alone), and a label row over formula cells that
// display numbers ("=ROW()-1" showing "1") was not marked as the header row
// because a formula never counted as a quantity. Both are the fold's calls.
//
// Code review finding (#400): the fold took the legacy formula arm for every
// formula cell, so the cached result the worker had sent (the number, the
// flag, the date) never reached the Document. A formula cell whose result
// the wire states keeps both: the source in formula_source, the result in
// the typed arm.

#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/office/v1/office_service.pb.h"
#include "grparse/docling_map.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace officev1 = ai::pipestream::office::v1;

namespace {

using grparse_test::require;
using grparse_test::require_equal;

officev1::StreamPagesResponse info_event(int sheets) {
  officev1::StreamPagesResponse event;
  officev1::DocumentInfo* info = event.mutable_document_info();
  info->set_document_id("book.xlsx");
  info->set_source_format("xlsx");
  info->set_page_count(sheets);
  info->set_document_type("spreadsheet");
  for (int i = 0; i < sheets; i++) {
    officev1::PageRect* page = info->add_page_rects();
    page->set_width_twips(24000);
    page->set_height_twips(15000);
  }
  return event;
}

officev1::StreamPagesResponse status_event() {
  officev1::StreamPagesResponse event;
  event.mutable_status()->set_state(officev1::RenderStatus::STATE_OK);
  return event;
}

officev1::StreamPagesResponse sheet_event(int index, const std::string& name, bool visible,
                                          int end_row, int end_column) {
  officev1::StreamPagesResponse event;
  officev1::Sheet* sheet = event.mutable_sheet();
  sheet->set_index(index);
  sheet->set_name(name);
  sheet->set_visible(visible);
  sheet->set_tab_color_rgb(-1);
  sheet->set_used_end_row(end_row);
  sheet->set_used_end_column(end_column);
  return event;
}

officev1::StreamPagesResponse row_event(int sheet_index, int row) {
  officev1::StreamPagesResponse event;
  event.mutable_sheet_row()->set_sheet_index(sheet_index);
  event.mutable_sheet_row()->set_row(row);
  return event;
}

void text_cell(officev1::SheetRow* row, int column, const std::string& text) {
  officev1::SheetCell* cell = row->add_cells();
  cell->set_column(column);
  cell->set_type(officev1::SHEET_CELL_TYPE_TEXT);
  cell->set_display(text);
  cell->set_merged_columns(1);
  cell->set_merged_rows(1);
}

void formula_cell(officev1::SheetRow* row, int column, const std::string& formula,
                  const std::string& display) {
  officev1::SheetCell* cell = row->add_cells();
  cell->set_column(column);
  cell->set_type(officev1::SHEET_CELL_TYPE_FORMULA);
  cell->set_formula(formula);
  cell->set_display(display);
  cell->set_merged_columns(1);
  cell->set_merged_rows(1);
}

// A formula cell from a worker that states what the formula evaluated to.
officev1::SheetCell* evaluated_formula_cell(officev1::SheetRow* row, int column,
                                            const std::string& formula,
                                            const std::string& display,
                                            officev1::SheetCellType result, double number = 0) {
  formula_cell(row, column, formula, display);
  officev1::SheetCell* cell = row->mutable_cells(row->cells_size() - 1);
  cell->set_formula_result(result);
  cell->set_number(number);
  return cell;
}

docv1::Document fold(const std::vector<officev1::StreamPagesResponse>& events) {
  grparse::DoclingMapper mapper;
  for (const auto& event : events) mapper.consume(event);
  if (!mapper.finished()) throw std::runtime_error("the stream did not finish");
  return mapper.document();
}

const docv1::TableCell* cell_at(const docv1::TableData& data, int row, int column) {
  for (const auto& cell : data.table_cells()) {
    if (cell.start_row_offset_idx() == row && cell.start_col_offset_idx() == column) return &cell;
  }
  return nullptr;
}

// A visible sheet with one cell, a hidden empty sheet, an empty visible
// sheet: one table each, sized by what streamed.
void verify_empty_sheets_fold_to_empty_tables() {
  std::vector<officev1::StreamPagesResponse> events{info_event(3)};
  events.push_back(sheet_event(0, "Visible", true, 0, 0));
  events.push_back(sheet_event(1, "Hidden", false, 0, 0));
  events.push_back(sheet_event(2, "Blank", true, 0, 0));
  events.push_back(row_event(0, 0));
  text_cell(events.back().mutable_sheet_row(), 0, "only");
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  require(document.tables_size() == 3, "one table per sheet");
  require(document.tables(0).data().num_rows() == 1 && document.tables(0).data().num_cols() == 1 &&
              document.tables(0).data().table_cells_size() == 1,
          "a sheet holding A1 alone is a 1 x 1 table with its cell");
  for (int index : {1, 2}) {
    const docv1::TableData& data = document.tables(index).data();
    require(data.table_cells_size() == 0 && data.num_rows() == 0 && data.num_cols() == 0,
            "a sheet that streamed no cell is an empty table, not a 1 x 1 grid with no cells");
  }
  require(document.groups_size() == 3 && document.groups(1).content_layer() == docv1::CONTENT_LAYER_INVISIBLE,
          "the hidden sheet keeps its group on the invisible layer");
  require(grparse::docling_integrity_errors(document).empty(), "the fold stays well formed");
}

// A label row above formula cells that display numbers is the header row.
void verify_formula_display_counts_as_a_quantity() {
  std::vector<officev1::StreamPagesResponse> events{info_event(1)};
  events.push_back(sheet_event(0, "Sheet1", true, 2, 2));
  events.push_back(row_event(0, 0));
  text_cell(events.back().mutable_sheet_row(), 0, "ID");
  text_cell(events.back().mutable_sheet_row(), 1, "Month");
  text_cell(events.back().mutable_sheet_row(), 2, "Pick");
  events.push_back(row_event(0, 1));
  formula_cell(events.back().mutable_sheet_row(), 0, "=ROW()-1", "1");
  text_cell(events.back().mutable_sheet_row(), 1, "January");
  formula_cell(events.back().mutable_sheet_row(), 2, "=B2", "January");
  events.push_back(row_event(0, 2));
  formula_cell(events.back().mutable_sheet_row(), 0, "=ROW()-1", "2");
  text_cell(events.back().mutable_sheet_row(), 1, "February");
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  require(document.tables_size() == 1, "one table");
  const docv1::TableData& data = document.tables(0).data();
  require(cell_at(data, 0, 0)->column_header() && cell_at(data, 0, 1)->column_header() &&
              cell_at(data, 0, 2)->column_header(),
          "the label row above numeric formula displays is the header row");
  require(!cell_at(data, 1, 0)->column_header(), "a formula row is not a header");
  require(cell_at(data, 1, 0)->value().formula() == "=ROW()-1", "the formula stays typed as a formula");
}

// A formula whose display is text is not a quantity, so a label row above
// text formulas alone is not a header.
void verify_text_formulas_do_not_make_a_header() {
  std::vector<officev1::StreamPagesResponse> events{info_event(1)};
  events.push_back(sheet_event(0, "Sheet1", true, 1, 1));
  events.push_back(row_event(0, 0));
  text_cell(events.back().mutable_sheet_row(), 0, "Name");
  text_cell(events.back().mutable_sheet_row(), 1, "Alias");
  events.push_back(row_event(0, 1));
  formula_cell(events.back().mutable_sheet_row(), 0, "=A9", "Ada");
  formula_cell(events.back().mutable_sheet_row(), 1, "=B9", "Countess");
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  const docv1::TableData& data = document.tables(0).data();
  require(!cell_at(data, 0, 0)->column_header(), "text formulas below do not make a header row");
}

// A formula whose result the wire states keeps its source and its cached
// result side by side, typed exactly as a value cell's would be.
void verify_formula_cell_keeps_its_cached_typed_result() {
  std::vector<officev1::StreamPagesResponse> events{info_event(1)};
  events.push_back(sheet_event(0, "Sheet1", true, 0, 4));
  events.push_back(row_event(0, 0));
  officev1::SheetRow* row = events.back().mutable_sheet_row();
  evaluated_formula_cell(row, 0, "=SUM(A2:A9)", "$1,234.50", officev1::SHEET_CELL_TYPE_VALUE,
                         1234.5)
      ->set_number_format_string("$#,##0.00");
  // A result of zero is a number like any other: the stated result type is
  // what tells it from a formula that yields text.
  evaluated_formula_cell(row, 1, "=A2-A2", "0", officev1::SHEET_CELL_TYPE_VALUE, 0);
  evaluated_formula_cell(row, 2, "=A2>A3", "TRUE", officev1::SHEET_CELL_TYPE_VALUE, 1)
      ->set_is_boolean(true);
  officev1::SheetCell* due = evaluated_formula_cell(row, 3, "=B2+30", "2023-04-15",
                                                    officev1::SHEET_CELL_TYPE_VALUE, 45031);
  due->set_is_datetime(true);
  due->mutable_datetime()->set_year(2023);
  due->mutable_datetime()->set_month(4);
  due->mutable_datetime()->set_day(15);
  evaluated_formula_cell(row, 4, "=A2&\" units\"", "7 units", officev1::SHEET_CELL_TYPE_TEXT);
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  const docv1::TableData& data = document.tables(0).data();

  const docv1::CellValue& total = cell_at(data, 0, 0)->value();
  require_equal(static_cast<int>(total.kind_case()), static_cast<int>(docv1::CellValue::kNumber),
                "a numeric formula's cached result is the cell's typed value");
  require_equal(total.number(), 1234.5, "the cached number is exact, not the display string");
  require_equal(total.formula_source(), std::string("=SUM(A2:A9)"),
                "the formula's source rides beside its result");
  require_equal(total.number_format(), std::string("$#,##0.00"), "the number format is kept");
  require_equal(cell_at(data, 0, 0)->text(), std::string("$1,234.50"),
                "the cell text stays the display string");

  const docv1::CellValue& zero = cell_at(data, 0, 1)->value();
  require(zero.kind_case() == docv1::CellValue::kNumber && zero.number() == 0
              && zero.formula_source() == "=A2-A2",
          "a formula that yields 0 is the number 0");

  const docv1::CellValue& flag = cell_at(data, 0, 2)->value();
  require(flag.kind_case() == docv1::CellValue::kBoolean && flag.boolean()
              && flag.formula_source() == "=A2>A3",
          "a logical result is a boolean");

  const docv1::CellValue& when = cell_at(data, 0, 3)->value();
  require(when.kind_case() == docv1::CellValue::kDatetime && when.datetime().year() == 2023
              && when.datetime().month() == 4 && when.datetime().day() == 15
              && when.formula_source() == "=B2+30",
          "a date result is a civil date");

  const docv1::TableCell* label = cell_at(data, 0, 4);
  require(label->has_value() && label->value().kind_case() == docv1::CellValue::KIND_NOT_SET
              && label->value().formula_source() == "=A2&\" units\"",
          "a text result claims no typed kind and keeps the formula's source");
  require_equal(label->text(), std::string("7 units"), "the text result is the cell text");
  require(grparse::docling_integrity_errors(document).empty(), "the fold stays well formed");
}

// A formula that failed still has a source: the error literal is its
// result, and the formula that produced it stays beside it.
void verify_failed_formula_keeps_its_source() {
  std::vector<officev1::StreamPagesResponse> events{info_event(1)};
  events.push_back(sheet_event(0, "Sheet1", true, 0, 0));
  events.push_back(row_event(0, 0));
  formula_cell(events.back().mutable_sheet_row(), 0, "=1/0", "#DIV/0!");
  events.back().mutable_sheet_row()->mutable_cells(0)->set_error_code(532);
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  const docv1::CellValue& value = cell_at(document.tables(0).data(), 0, 0)->value();
  require_equal(value.error(), std::string("#DIV/0!"), "the error literal is the result");
  require_equal(value.formula_source(), std::string("=1/0"),
                "the failed formula's source is kept");
}

// A worker that predates SheetCell.formula_result does not say what a
// formula evaluated to: number reads 0 for a text result as for a zero, and
// its date and flag marks followed the column's format, not the result. Such
// a cell stays on the legacy formula arm rather than claim a guessed value.
void verify_formula_without_a_stated_result_stays_legacy() {
  std::vector<officev1::StreamPagesResponse> events{info_event(1)};
  events.push_back(sheet_event(0, "Sheet1", true, 0, 1));
  events.push_back(row_event(0, 0));
  officev1::SheetRow* row = events.back().mutable_sheet_row();
  formula_cell(row, 0, "=A9", "Ada");
  formula_cell(row, 1, "=B9", "pending");
  row->mutable_cells(1)->set_is_datetime(true);
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  const docv1::TableData& data = document.tables(0).data();
  for (int column : {0, 1}) {
    const docv1::CellValue& value = cell_at(data, 0, column)->value();
    require(value.kind_case() == docv1::CellValue::kFormula && !value.has_formula_source(),
            "a formula with no stated result keeps the legacy formula arm");
  }
  require_equal(cell_at(data, 0, 0)->value().formula(), std::string("=A9"),
                "the legacy arm carries the formula");
}

// A formula's typed result decides what the header band rule sees: a
// numeric result is a quantity whatever its display looks like, and a text
// result is a label.
void verify_typed_formula_results_shape_the_header_band() {
  std::vector<officev1::StreamPagesResponse> events{info_event(1)};
  events.push_back(sheet_event(0, "Sheet1", true, 2, 1));
  events.push_back(row_event(0, 0));
  evaluated_formula_cell(events.back().mutable_sheet_row(), 0, "=Setup!A1", "Region",
                         officev1::SHEET_CELL_TYPE_TEXT);
  evaluated_formula_cell(events.back().mutable_sheet_row(), 1, "=Setup!B1", "Revenue",
                         officev1::SHEET_CELL_TYPE_TEXT);
  for (int data_row : {1, 2}) {
    events.push_back(row_event(0, data_row));
    text_cell(events.back().mutable_sheet_row(), 0, data_row == 1 ? "North" : "South");
    evaluated_formula_cell(events.back().mutable_sheet_row(), 1, "=SUM(C2:F2)", "$1,234.50",
                           officev1::SHEET_CELL_TYPE_VALUE, 1234.5);
  }
  events.push_back(status_event());
  const docv1::Document document = fold(events);
  const docv1::TableData& data = document.tables(0).data();
  require(cell_at(data, 0, 0)->column_header() && cell_at(data, 0, 1)->column_header(),
          "text formulas over numeric formulas are the header row");
  require(!cell_at(data, 1, 1)->column_header() && !cell_at(data, 2, 1)->column_header(),
          "the numeric formula rows are data");
}

// Sheets and pivots own no rectangle: their provenance names the sheet grid
// and never presents a zero-area box as real geometry, and the absence is
// named once per item.
void verify_sheet_provenance_and_pivot_span() {
  grparse::DoclingMapper mapper;
  mapper.consume(info_event(1));
  mapper.consume(sheet_event(0, "Data", true, 1, 1));
  officev1::StreamPagesResponse row = row_event(0, 0);
  text_cell(row.mutable_sheet_row(), 0, "only");
  mapper.consume(row);
  officev1::StreamPagesResponse pivot;
  officev1::SheetPivotTable* table = pivot.mutable_sheet_pivot_table();
  table->set_sheet_index(0);
  table->set_name("Summary");
  table->mutable_source_range()->set_start_row(0);
  table->mutable_source_range()->set_start_column(0);
  table->mutable_source_range()->set_end_row(1);
  table->mutable_source_range()->set_end_column(1);
  table->mutable_output_range()->set_start_row(5);
  table->mutable_output_range()->set_start_column(0);
  table->mutable_output_range()->set_end_row(6);
  table->mutable_output_range()->set_end_column(1);
  mapper.consume(pivot);
  mapper.consume(status_event());

  const docv1::Document& document = mapper.document();
  const docv1::ProvenanceItem& sheet_prov = document.tables(0).prov(0);
  require(sheet_prov.page_no() == 1 && !sheet_prov.has_bbox() &&
              sheet_prov.grid().sheet() == "Data",
          "a sheet's provenance is page and grid, no fabricated box");
  require(document.tables_size() == 1,
          "a pivot adds no empty table: its output cells are the sheet's own");
  require(document.pivots_size() == 1 && document.pivots(0).output().start().row() == 5
              && document.pivots(0).output().start().sheet() == "Data",
          "the pivot's output is a grid span into its sheet");
  require(mapper.warnings().size() == 1 && mapper.warnings()[0].contains("has no geometry"),
          "the sheet's missing geometry is named once");
  require(grparse::docling_integrity_errors(document).empty(), "the fold stays well formed");
}

// S3 eval finding: a picture on a sheet's draw page arrives keyed to the
// sheet index with no anchor, and was placed under the body with no
// provenance at all. It belongs under its sheet's group, with the sheet's
// own honest provenance: the page and the sheet, no box.
void verify_sheet_picture_sits_under_its_sheet() {
  grparse::DoclingMapper mapper;
  mapper.consume(info_event(2));
  mapper.consume(sheet_event(0, "Cover", true, 0, 0));
  mapper.consume(sheet_event(1, "Logo", true, 0, 0));
  officev1::StreamPagesResponse image;
  officev1::EmbeddedImage* picture = image.mutable_embedded_image();
  picture->set_index(0);
  picture->set_page_index(1);
  picture->set_name("Picture 1");
  picture->set_width_twips(1440);
  picture->set_height_twips(720);
  mapper.consume(image);
  mapper.consume(status_event());

  const docv1::Document& document = mapper.document();
  require(document.pictures_size() == 1, "the picture is placed once");
  const docv1::PictureItem& placed = document.pictures(0);
  require(placed.parent().ref() == document.groups(1).self_ref(),
          "the picture sits under the sheet it was drawn on");
  require(placed.prov_size() == 1 && placed.prov(0).page_no() == 2
              && !placed.prov(0).has_bbox() && placed.prov(0).grid().sheet() == "Logo",
          "its provenance names the sheet's page and grid, no fabricated box");
  require(grparse::docling_integrity_errors(document).empty(), "the fold stays well formed");
}

// S3 eval finding: a title merged across twelve columns over a four-column
// used range came out as a cell outside its own table's grid.
void verify_merge_past_used_range_grows_the_table() {
  grparse::DoclingMapper mapper;
  mapper.consume(info_event(1));
  mapper.consume(sheet_event(0, "Report", true, 1, 3));
  officev1::StreamPagesResponse title = row_event(0, 0);
  text_cell(title.mutable_sheet_row(), 0, "Quarterly report");
  title.mutable_sheet_row()->mutable_cells(0)->set_merged_columns(12);
  title.mutable_sheet_row()->mutable_cells(0)->set_merged_rows(3);
  mapper.consume(title);
  mapper.consume(status_event());

  const docv1::TableData& data = mapper.document().tables(0).data();
  require(data.num_cols() == 12 && data.num_rows() == 3,
          "the table grows to hold the merge");
  require(grparse::docling_integrity_errors(mapper.document()).empty(),
          "the fold stays well formed");
}

// "row R, col C" of a table's origin, for a failure message.
std::string origin_text(const docv1::TableData& data) {
  return "row " + std::to_string(data.origin().row()) + ", col " +
         std::to_string(data.origin().col());
}

// A number cell as the worker sends one.
void number_cell(officev1::SheetRow* row, int column, double number,
                 const std::string& display) {
  officev1::SheetCell* cell = row->add_cells();
  cell->set_column(column);
  cell->set_type(officev1::SHEET_CELL_TYPE_VALUE);
  cell->set_number(number);
  cell->set_display(display);
  cell->set_merged_columns(1);
  cell->set_merged_rows(1);
}

// A sheet whose used range is C5:E7, with widths for columns A to E. The
// caller adds the rows.
officev1::StreamPagesResponse offset_sheet_event(int index, const std::string& name) {
  officev1::StreamPagesResponse event = sheet_event(index, name, true, 6, 4);
  event.mutable_sheet()->set_used_start_row(4);
  event.mutable_sheet()->set_used_start_column(2);
  for (int column = 0; column < 5; column++) {
    event.mutable_sheet()->add_column_widths_twips(1000 + column);
  }
  return event;
}

// The shared schema gives TableData an origin for a table cut out of a
// larger grid, and names the sheet as its case: a sheet whose used range
// starts at C5 holds the used range only, with grid cell (0, 0) at C5 and
// the origin saying so. The fold used to write the cells at their sheet
// addresses, so a sheet whose data sat far from A1 was a table of blank
// leading rows and columns, and the renderers' grid budget cut the data off
// the end of it.
void verify_a_sheet_table_holds_its_used_range_only() {
  grparse::DoclingMapper mapper;
  mapper.consume(info_event(2));
  mapper.consume(offset_sheet_event(0, "Stock"));
  officev1::StreamPagesResponse labels = row_event(0, 4);
  text_cell(labels.mutable_sheet_row(), 2, "Region");
  text_cell(labels.mutable_sheet_row(), 3, "Units");
  text_cell(labels.mutable_sheet_row(), 4, "Price");
  mapper.consume(labels);
  officev1::StreamPagesResponse north = row_event(0, 5);
  text_cell(north.mutable_sheet_row(), 2, "North");
  number_cell(north.mutable_sheet_row(), 3, 12, "12");
  number_cell(north.mutable_sheet_row(), 4, 3.5, "3.50");
  mapper.consume(north);
  officev1::StreamPagesResponse south = row_event(0, 6);
  text_cell(south.mutable_sheet_row(), 2, "South");
  number_cell(south.mutable_sheet_row(), 3, 7, "7");
  number_cell(south.mutable_sheet_row(), 4, 4.25, "4.25");
  mapper.consume(south);
  // The database range declares the header row, in sheet addresses.
  officev1::StreamPagesResponse declared;
  officev1::SheetDatabaseRange* range = declared.mutable_sheet_database_range();
  range->set_name("StockList");
  range->set_sheet_index(0);
  range->mutable_range()->set_start_row(4);
  range->mutable_range()->set_start_column(2);
  range->mutable_range()->set_end_row(6);
  range->mutable_range()->set_end_column(4);
  range->set_contains_header(true);
  mapper.consume(declared);
  // A second sheet that starts at A1.
  mapper.consume(sheet_event(1, "Plain", true, 0, 0));
  officev1::StreamPagesResponse plain = row_event(1, 0);
  text_cell(plain.mutable_sheet_row(), 0, "only");
  mapper.consume(plain);
  mapper.consume(status_event());

  const docv1::Document& document = mapper.document();
  const docv1::TableData& data = document.tables(0).data();
  require(data.has_origin() && data.origin().row() == 4 && data.origin().col() == 2,
          "the table's origin is the used range's first cell, C5, got " +
              origin_text(data));
  require_equal(data.num_rows(), 3, "rows of the used range");
  require_equal(data.num_cols(), 3, "columns of the used range");
  const docv1::TableCell* region = cell_at(data, 0, 0);
  require(region != nullptr && region->text() == "Region",
          "grid cell (0, 0) is the used range's first cell");
  require(region != nullptr && region->column_header() && cell_at(data, 0, 2) != nullptr &&
              cell_at(data, 0, 2)->column_header(),
          "the database range's header row is marked where it sits in the cut grid");
  const docv1::TableCell* units = cell_at(data, 1, 1);
  require(units != nullptr && units->value().number() == 12 && !units->column_header(),
          "a data cell sits at its sheet address minus the origin");
  for (const docv1::TableCell& cell : data.table_cells()) {
    require(cell.end_row_offset_idx() <= data.num_rows() &&
                cell.end_col_offset_idx() <= data.num_cols(),
            "every cell is inside the grid");
  }
  require(data.columns_size() == 3 && data.columns(0).name() == "C" &&
              data.columns(0).width() == 1002 && data.columns(2).name() == "E",
          "the column schema starts at the origin column");
  require(data.row_prov_size() == 3 && data.row_prov(0).grid().row() == 4 &&
              data.row_prov(0).grid().col() == 2,
          "row provenance keeps the sheet's own addresses");
  require(document.named_ranges_size() == 1 &&
              document.named_ranges(0).range().start().row() == 4 &&
              document.named_ranges(0).range().start().col() == 2,
          "a range of the workbook keeps the sheet's own addresses");
  const docv1::TableData& plain_data = document.tables(1).data();
  require(!plain_data.has_origin() && cell_at(plain_data, 0, 0) != nullptr,
          "a sheet that starts at A1 states no origin");
  require(grparse::docling_integrity_errors(document).empty(), "the fold stays well formed");
}

// A lone merged label across the used width at the top is a section row.
// The used width is the width of the used range, wherever the range starts:
// a title over C5:E5 spans its three-column table whole.
void verify_a_title_across_the_used_width_is_a_section_row_wherever_the_range_starts() {
  grparse::DoclingMapper mapper;
  mapper.consume(info_event(1));
  officev1::StreamPagesResponse sheet = offset_sheet_event(0, "Stock");
  sheet.mutable_sheet()->set_used_end_row(7);
  mapper.consume(sheet);
  officev1::StreamPagesResponse title = row_event(0, 4);
  text_cell(title.mutable_sheet_row(), 2, "Stock on hand");
  title.mutable_sheet_row()->mutable_cells(0)->set_merged_columns(3);
  mapper.consume(title);
  officev1::StreamPagesResponse labels = row_event(0, 5);
  text_cell(labels.mutable_sheet_row(), 2, "Region");
  text_cell(labels.mutable_sheet_row(), 3, "Units");
  text_cell(labels.mutable_sheet_row(), 4, "Price");
  mapper.consume(labels);
  for (int row = 6; row < 8; row++) {
    officev1::StreamPagesResponse line = row_event(0, row);
    text_cell(line.mutable_sheet_row(), 2, row == 6 ? "North" : "South");
    number_cell(line.mutable_sheet_row(), 3, row, std::to_string(row));
    number_cell(line.mutable_sheet_row(), 4, row + 0.5, std::to_string(row) + ".50");
    mapper.consume(line);
  }
  mapper.consume(status_event());

  const docv1::TableData& data = mapper.document().tables(0).data();
  const docv1::TableCell* title_cell = cell_at(data, 0, 0);
  require(title_cell != nullptr && title_cell->text() == "Stock on hand" &&
              title_cell->row_section(),
          "the title across the used width is a section row");
  require(cell_at(data, 1, 0) != nullptr && cell_at(data, 1, 0)->column_header() &&
              cell_at(data, 1, 2) != nullptr && cell_at(data, 1, 2)->column_header(),
          "the labels under it are the header row");
  require(cell_at(data, 2, 1) != nullptr && !cell_at(data, 2, 1)->column_header(),
          "the data under the labels is not");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("docling_map_sheet_shape_test", "ok", {
      verify_empty_sheets_fold_to_empty_tables,
      verify_formula_display_counts_as_a_quantity,
      verify_text_formulas_do_not_make_a_header,
      verify_formula_cell_keeps_its_cached_typed_result,
      verify_failed_formula_keeps_its_source,
      verify_formula_without_a_stated_result_stays_legacy,
      verify_typed_formula_results_shape_the_header_band,
      verify_sheet_provenance_and_pivot_span,
      verify_sheet_picture_sits_under_its_sheet,
      verify_merge_past_used_range_grows_the_table,
      verify_a_sheet_table_holds_its_used_range_only,
      verify_a_title_across_the_used_width_is_a_section_row_wherever_the_range_starts,
  });
}
