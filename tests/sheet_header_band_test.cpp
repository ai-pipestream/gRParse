// S3 eval finding against the sheet fold: report-style government sheets
// put a title, a "RUN AT" line and blank rows before the header, and the
// header itself is often two lines (a group line over a leaf line). The
// old rule read only the first row, so none of those sheets carried a
// column_header. Each case here is one shape from that corpus, fed as
// synthetic rows through the whole fold, plus the shapes the rule must
// keep refusing: a numeric first row, a title adjacent to the header, a
// record that merely lacks its number, and a contents list without
// numbers.

#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/office/v1/office_service.pb.h"
#include "grparse/docling_map.h"
#include "grparse/office_fold/sheet_header_band.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace officev1 = ai::pipestream::office::v1;

namespace {

using grparse_test::require;

// One sheet under construction: rows are appended in order and the
// workbook folds on finish().
class Sheet {
 public:
  Sheet(int end_row, int end_column) {
    officev1::StreamPagesResponse info;
    officev1::DocumentInfo* document = info.mutable_document_info();
    document->set_document_id("report.xlsx");
    document->set_source_format("xlsx");
    document->set_page_count(1);
    document->set_document_type("spreadsheet");
    officev1::PageRect* page = document->add_page_rects();
    page->set_width_twips(24000);
    page->set_height_twips(15000);
    events_.push_back(info);
    officev1::StreamPagesResponse sheet;
    sheet.mutable_sheet()->set_index(0);
    sheet.mutable_sheet()->set_name("Table 1");
    sheet.mutable_sheet()->set_visible(true);
    sheet.mutable_sheet()->set_tab_color_rgb(-1);
    sheet.mutable_sheet()->set_used_end_row(end_row);
    sheet.mutable_sheet()->set_used_end_column(end_column);
    events_.push_back(sheet);
  }

  // Labels at consecutive columns from `first_column`.
  Sheet& labels(int row, int first_column, const std::vector<std::string>& texts) {
    officev1::SheetRow* out = open_row(row);
    int column = first_column;
    for (const std::string& text : texts) text_cell(out, column++, text, 1);
    return *this;
  }

  // One label merged over `span` columns.
  Sheet& merged(int row, int column, const std::string& text, int span) {
    text_cell(open_row(row), column, text, span);
    return *this;
  }

  // Numbers at consecutive columns from `first_column`.
  Sheet& numbers(int row, int first_column, const std::vector<double>& values) {
    officev1::SheetRow* out = open_row(row);
    int column = first_column;
    for (const double value : values) {
      officev1::SheetCell* cell = out->add_cells();
      cell->set_column(column++);
      cell->set_type(officev1::SHEET_CELL_TYPE_VALUE);
      cell->set_number(value);
      cell->set_display(std::to_string(value));
      cell->set_merged_columns(1);
      cell->set_merged_rows(1);
    }
    return *this;
  }

  docv1::Document finish() {
    officev1::StreamPagesResponse status;
    status.mutable_status()->set_state(officev1::RenderStatus::STATE_OK);
    events_.push_back(status);
    grparse::DoclingMapper mapper;
    for (const auto& event : events_) mapper.consume(event);
    if (!mapper.finished()) throw std::runtime_error("the stream did not finish");
    return mapper.document();
  }

 private:
  officev1::SheetRow* open_row(int row) {
    if (!events_.empty() && events_.back().has_sheet_row()
        && events_.back().sheet_row().row() == row) {
      return events_.back().mutable_sheet_row();
    }
    officev1::StreamPagesResponse event;
    event.mutable_sheet_row()->set_sheet_index(0);
    event.mutable_sheet_row()->set_row(row);
    events_.push_back(event);
    return events_.back().mutable_sheet_row();
  }

  static void text_cell(officev1::SheetRow* row, int column, const std::string& text, int span) {
    officev1::SheetCell* cell = row->add_cells();
    cell->set_column(column);
    cell->set_type(officev1::SHEET_CELL_TYPE_TEXT);
    cell->set_display(text);
    cell->set_merged_columns(span);
    cell->set_merged_rows(1);
  }

  std::vector<officev1::StreamPagesResponse> events_;
};

// The rows whose cells all carry column_header, ascending.
std::vector<int> header_rows(const docv1::Document& document) {
  std::vector<int> rows;
  for (const docv1::TableCell& cell : document.tables(0).data().table_cells()) {
    if (!cell.column_header()) continue;
    if (rows.empty() || rows.back() != cell.start_row_offset_idx()) {
      rows.push_back(cell.start_row_offset_idx());
    }
  }
  return rows;
}

std::string rows_text(const std::vector<int>& rows) {
  std::string out = "[";
  for (const int row : rows) out += (out.size() > 1 ? "," : "") + std::to_string(row);
  return out + "]";
}

void require_header(const docv1::Document& document, const std::vector<int>& expected,
                    const std::string& what) {
  const std::vector<int> actual = header_rows(document);
  require(actual == expected, what + ": expected header rows " + rows_text(expected) + ", got "
                                  + rows_text(actual));
}

// A title, blank rows, a "RUN AT" line, then the header over the data.
void verify_title_and_preamble_are_skipped() {
  const docv1::Document document = Sheet(9, 6)
      .labels(0, 0, {"A3131"}).labels(0, 6, {"Expenditure over 25k"})
      .labels(5, 0, {"RUN AT 05/01/2015"})
      .labels(6, 0, {"Department", "Entity", "Date", "Expense type", "Supplier", "Amount", "VAT"})
      .labels(7, 0, {"DH", "NHS", "31/12/2014", "Clinical", "Arriva"}).numbers(7, 5, {69447.64, 6242612})
      .labels(8, 0, {"DH", "NHS", "31/12/2014", "Clinical", "BMI"}).numbers(8, 5, {40674.45, 6024965})
      .labels(9, 0, {"DH", "NHS", "31/12/2014", "Clinical", "Circle"}).numbers(9, 5, {-26284.93, 6195098})
      .finish();
  require_header(document, {6}, "title, gap and run-at line are preamble");
  require(!document.tables(0).data().table_cells(0).column_header()
              && !document.tables(0).data().table_cells(0).row_section(),
          "a two-cell title row is neither a header nor a section row");
}

// A group line merged over column pairs, then the leaf line, then data:
// both lines are the header.
void verify_two_line_header_is_one_band() {
  const docv1::Document document = Sheet(6, 4)
      .merged(0, 0, "Figure 5: Estimated population by age", 4)
      .labels(2, 0, {"Age"}).merged(2, 1, "absolute numbers", 2).merged(2, 3, "('000s)", 2)
      .labels(3, 1, {"Males", "Females", "Males", "Females"})
      .numbers(4, 0, {0, 29161, 27897, 29, 28})
      .numbers(5, 0, {1, 30258, 28640, 30, 29})
      .numbers(6, 0, {2, 31069, 29693, 31, 30})
      .finish();
  require_header(document, {2, 3}, "a group line and its leaf line");
}

// A title directly above the band with two narrow cells stays outside it,
// while the group and leaf lines under it are the header.
void verify_adjacent_narrow_title_stays_out() {
  const docv1::Document document = Sheet(5, 7)
      .labels(0, 0, {"Table 1  Estimated population"}).labels(0, 7, {"MALES"})
      .labels(1, 0, {"Area"}).labels(1, 4, {"Age group"})
      .labels(2, 1, {"All ages", "0 - 4", "5 - 9", "10 - 14", "15 - 19", "20 - 24", "25 - 29"})
      .labels(3, 0, {"SCOTLAND"}).numbers(3, 1, {2446248, 134505, 148674, 195527, 135357, 164301, 170000})
      .labels(4, 0, {"Aberdeen"}).numbers(4, 1, {211910, 10239, 10860, 11212, 13777, 19608, 20000})
      .labels(5, 0, {"Angus"}).numbers(5, 1, {108370, 5713, 6495, 6782, 6505, 5245, 5300})
      .finish();
  require_header(document, {2}, "narrow title and group rows stay out; the leaf line is the header");
}

// Section labels and a two-line record between the header and the first
// numbers do not hide the header. The wrapped second line ("Codes / and
// substance / sexes") is sparser than the line above, which is also what a
// record without its number looks like, so it stays out: a partial header
// over a wrong one.
void verify_section_rows_between_header_and_data_are_skipped() {
  const docv1::Document document = Sheet(8, 5)
      .labels(0, 0, {"Table 6.12  Deaths by cause"})
      .labels(1, 0, {"ICD", "Cause of death", "Both", "Males", "Females", "Rate"})
      .labels(2, 0, {"Codes", "and substance", "sexes"})
      .labels(3, 0, {"ACCIDENTS"})
      .labels(6, 0, {"X40", "Accidental poisoning"})
      .labels(7, 0, {"-49", "to noxious substances"}).numbers(7, 2, {50, 44, 6, 1.2})
      .labels(8, 0, {"X41", "Antiepileptic drugs"}).numbers(8, 2, {12, 8, 4, 0.3})
      .finish();
  require_header(document, {1}, "section label and two-line record are preamble");
}

// A header at row 0 over numbers: the plain case keeps working.
void verify_plain_header_at_first_row() {
  const docv1::Document document = Sheet(3, 2)
      .labels(0, 0, {"Region", "Q1", "Q2"})
      .labels(1, 0, {"North"}).numbers(1, 1, {12, 14})
      .labels(2, 0, {"South"}).numbers(2, 1, {9, 11})
      .labels(3, 0, {"East"}).numbers(3, 1, {7, 8})
      .finish();
  require_header(document, {0}, "a plain sheet");
}

// Numbers from the first row: nothing to call a header.
void verify_numeric_first_row_has_no_header() {
  const docv1::Document document = Sheet(2, 2)
      .numbers(0, 0, {1, 2, 3})
      .numbers(1, 0, {4, 5, 6})
      .numbers(2, 0, {7, 8, 9})
      .finish();
  require_header(document, {}, "a numeric first row");
}

// A text record that merely lacks its number sits between the header and
// the first quantity: it is not a second header line.
void verify_record_without_number_is_not_a_header_line() {
  const docv1::Document document = Sheet(4, 5)
      .labels(0, 0, {"Director", "Contract", "Title", "Manager", "Supplier", "Value"})
      .labels(1, 0, {"M. Pocock", "To be tendered", "Procurement", "L. Deer"})
      .labels(2, 0, {"M. Pocock", "C005032", "Office", "D. Murray", "Muse"}).numbers(2, 5, {8000000})
      .labels(3, 0, {"M. Pocock", "To be tendered", "Ergonomics", "D. Murray"})
      .labels(4, 0, {"M. Pocock", "C005033", "Fixtures", "D. Murray", "Herman"}).numbers(4, 5, {900000})
      .finish();
  require_header(document, {0}, "a record without its number stays data");
}

// A sheet without any quantity: a full-width first row over consecutive
// label rows is the header.
void verify_all_text_table_marks_its_first_row() {
  const docv1::Document document = Sheet(3, 2)
      .labels(0, 0, {"Country", "Institution", "Type"})
      .labels(1, 0, {"Austria", "Institute for Parasitology", "Federal"})
      .labels(2, 0, {"Belgium", "Institute of Tropical Medicine", "Institute"})
      .labels(3, 0, {"Bosnia", "University of Sarajevo", "University"})
      .finish();
  require_header(document, {0}, "an all-text table");
}

// A contents list with gaps and no full-width first row claims no header.
void verify_all_text_contents_list_has_no_header() {
  const docv1::Document document = Sheet(6, 2)
      .labels(0, 0, {"JH", "Page 1 Table of contents"})
      .labels(3, 0, {"VT", "Table 2", "Breaks by breakdown"})
      .labels(4, 0, {"BT"}).labels(4, 2, {"Base: all respondents"})
      .labels(6, 0, {"VT", "Table 4", "Summary"})
      .finish();
  require_header(document, {}, "a contents list");
}

// The pure rule, on row shapes: a record with the header's label count
// does not join the band, a group line always carries its leaf line, and a
// band never runs past four lines.
void verify_band_rule_on_shapes() {
  using grparse::office_fold::SheetRowShape;
  using grparse::office_fold::header_band_rows;
  auto row = [](int index, int labels, int quantities, int coverage, bool group) {
    SheetRowShape shape;
    shape.index = index;
    shape.labels = labels;
    shape.quantities = quantities;
    shape.coverage = coverage;
    shape.group_line = group;
    return shape;
  };
  require(header_band_rows({row(0, 6, 0, 6, false), row(1, 6, 0, 6, false), row(2, 1, 5, 6, false)})
              == std::vector<int>{0},
          "a second line with the same label count is a record, not a header line");
  require(header_band_rows({row(0, 3, 0, 6, true), row(1, 3, 0, 6, false), row(2, 1, 5, 6, false)})
              == std::vector<int>{0, 1},
          "a group line carries the line under it");
  std::vector<SheetRowShape> deep;
  for (int i = 0; i < 6; i++) deep.push_back(row(i, 2 + i, 0, 8, false));
  deep.push_back(row(6, 1, 7, 8, false));
  require(header_band_rows(deep) == (std::vector<int>{0, 1, 2, 3}), "the band stops at four lines");
  require(header_band_rows({row(0, 2, 1, 3, false), row(2, 5, 0, 8, false), row(3, 1, 7, 8, false)})
              == std::vector<int>{2},
          "a narrow row with a stray number is preamble, not the data start");
  require(header_band_rows({row(0, 2, 0, 8, true), row(2, 5, 0, 8, false), row(3, 1, 7, 8, false)})
              == std::vector<int>{2},
          "a blank row breaks the run: the title above the gap stays out");
  require(header_band_rows({row(0, 5, 0, 5, false), row(1, 5, 1, 6, false), row(2, 5, 1, 6, false)})
              .empty(),
          "a band no denser than the data row is a record, so nothing is marked");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("sheet_header_band_test", "ok", {
      verify_title_and_preamble_are_skipped,
      verify_two_line_header_is_one_band,
      verify_adjacent_narrow_title_stays_out,
      verify_section_rows_between_header_and_data_are_skipped,
      verify_plain_header_at_first_row,
      verify_numeric_first_row_has_no_header,
      verify_record_without_number_is_not_a_header_line,
      verify_all_text_table_marks_its_first_row,
      verify_all_text_contents_list_has_no_header,
      verify_band_rule_on_shapes,
  });
}
