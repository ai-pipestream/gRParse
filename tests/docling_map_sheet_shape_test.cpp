// S3 eval findings against the sheet fold: an empty or hidden sheet arrived
// as a 1 x 1 table with no cells (its used range reads 0,0 whether the sheet
// is empty or holds A1 alone), and a label row over formula cells that
// display numbers ("=ROW()-1" showing "1") was not marked as the header row
// because a formula never counted as a quantity. Both are the fold's calls.

#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/office/v1/office_service.pb.h"
#include "grparse/docling_map.h"

namespace docv1 = ai::pipestream::document::v1;
namespace officev1 = ai::pipestream::office::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

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

}  // namespace

int main() {
  try {
    verify_empty_sheets_fold_to_empty_tables();
    verify_formula_display_counts_as_a_quantity();
    verify_text_formulas_do_not_make_a_header();
  } catch (const std::exception& error) {
    std::println(stderr, "docling_map_sheet_shape_test: {}", error.what());
    return 1;
  }
  std::println("docling_map_sheet_shape_test: ok");
  return 0;
}
