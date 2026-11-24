// Calamine is the primary reader of workbook cells: a routed workbook's
// sheet tables keep libreoffice's item (its place, its refs) and take
// calamine's cells. Each case builds the two collector documents by hand,
// the way the two folds shape them, and checks the merged result: values
// and text are calamine's, a merged area libreoffice saw survives when
// calamine reports none, calamine's own merges win when it has them, the
// header band is marked over the adopted cells, a sheet with no table to
// land in is named, not dropped silently, and a sheet calamine did not read
// to its end keeps libreoffice's cells under a typed warning.

#include <print>
#include <string>
#include <vector>

#include <google/protobuf/util/message_differencer.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/collector_coordinator.h"
#include "grparse/workbook_cells.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

namespace {

using grparse_test::require;
using grparse_test::require_equal;

// A workbook document with one sheet group per name, each holding one
// table, built the way both folds lay a sheet out.
class Workbook {
 public:
  explicit Workbook(std::string collector) : collector_(std::move(collector)) {}

  docv1::TableData* sheet(const std::string& name) {
    const int group_index = document_.groups_size();
    docv1::GroupItem* group = document_.add_groups();
    group->set_self_ref("#/groups/" + std::to_string(group_index));
    group->mutable_parent()->set_ref("#/body");
    group->set_name(name);
    group->set_label(docv1::GROUP_LABEL_SHEET);
    document_.mutable_body()->add_children()->set_ref(group->self_ref());
    docv1::TableItem* table = document_.add_tables();
    table->set_self_ref("#/tables/" + std::to_string(document_.tables_size() - 1));
    table->mutable_parent()->set_ref(group->self_ref());
    table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
    table->add_source()->mutable_collector()->set_collector(collector_);
    group->add_children()->set_ref(table->self_ref());
    return table->mutable_data();
  }

  docv1::Document& document() { return document_; }

 private:
  std::string collector_;
  docv1::Document document_;
};

docv1::TableCell* text(docv1::TableData* data, int row, int column, const std::string& value,
                       int columns = 1) {
  docv1::TableCell* cell = data->add_table_cells();
  cell->set_start_row_offset_idx(row);
  cell->set_end_row_offset_idx(row + 1);
  cell->set_start_col_offset_idx(column);
  cell->set_end_col_offset_idx(column + columns);
  cell->set_row_span(1);
  cell->set_col_span(columns);
  cell->set_text(value);
  data->set_num_rows(std::max(data->num_rows(), row + 1));
  data->set_num_cols(std::max(data->num_cols(), column + columns));
  return cell;
}

docv1::TableCell* number(docv1::TableData* data, int row, int column, double value,
                         const std::string& display) {
  docv1::TableCell* cell = text(data, row, column, display);
  cell->mutable_value()->set_number(value);
  return cell;
}

const docv1::TableCell* at(const docv1::TableData& data, int row, int column) {
  for (const docv1::TableCell& cell : data.table_cells()) {
    if (cell.start_row_offset_idx() == row && cell.start_col_offset_idx() == column) {
      return &cell;
    }
  }
  return nullptr;
}

// A report sheet as libreoffice folds it: a merged title over the width, a
// header, and records whose amounts it displays with two decimals and whose
// reference text it turned into a date.
void report_libre(docv1::TableData* data) {
  text(data, 0, 0, "Expenditure over threshold", 3);
  text(data, 1, 0, "Supplier")->set_column_header(true);
  text(data, 1, 1, "Reference")->set_column_header(true);
  text(data, 1, 2, "Amount")->set_column_header(true);
  text(data, 2, 0, "NEMS");
  docv1::TableCell* misread = text(data, 2, 1, "2016-01-31");
  misread->mutable_value()->mutable_datetime()->set_year(2016);
  number(data, 2, 2, 3495, "3495.00")->mutable_value()->set_number_format("0.00");
  text(data, 3, 0, "BMI");
  text(data, 3, 1, "1-31-16");
  number(data, 3, 2, 30697.43, "30697.43")->mutable_value()->set_number_format("0.00");
}

// The same sheet as calamine reads it: the stored values, no merge (an ODS
// read reports none), the reference text as written.
void report_calamine(docv1::TableData* data) {
  text(data, 0, 0, "Expenditure over threshold");
  text(data, 1, 0, "Supplier");
  text(data, 1, 1, "Reference");
  text(data, 1, 2, "Amount");
  text(data, 2, 0, "NEMS");
  text(data, 2, 1, "1-31-16");
  number(data, 2, 2, 3495, "3495");
  text(data, 3, 0, "BMI");
  text(data, 3, 1, "1-31-16");
  number(data, 3, 2, 30697.43, "30697.43");
  data->set_num_cols(3);
}

void verify_values_and_text_are_calamine() {
  Workbook libre("libreoffice");
  report_libre(libre.sheet("Report"));
  Workbook calamine("calamine");
  report_calamine(calamine.sheet("Report"));
  std::vector<grparse::Warning> warnings;
  const int adopted =
      grparse::adopt_calamine_cells(calamine.document(), {}, &libre.document(), &warnings);
  require_equal(adopted, 1, "sheet tables adopted");
  require(warnings.empty(), "no warning for a sheet both read");
  const docv1::TableItem& table = libre.document().tables(0);
  const docv1::TableCell* reference = at(table.data(), 2, 1);
  require(reference != nullptr, "the reference cell is placed");
  require_equal(reference->text(), std::string("1-31-16"), "reference text");
  require(!reference->has_value(), "a text cell carries no date value");
  const docv1::TableCell* amount = at(table.data(), 2, 2);
  require_equal(amount->text(), std::string("3495.00"), "equal numeric value keeps the source display");
  require_equal(amount->value().number(), 3495.0, "amount value");
  require_equal(amount->value().number_format(), std::string("0.00"),
                "libreoffice's number format stays");
  require_equal(table.source(0).collector().collector(), std::string("calamine"),
                "the table names calamine first");
  require_equal(table.source(1).collector().collector(), std::string("libreoffice"),
                "and keeps libreoffice");
}

void verify_libre_merges_fill_in_for_calamine() {
  Workbook libre("libreoffice");
  report_libre(libre.sheet("Report"));
  Workbook calamine("calamine");
  report_calamine(calamine.sheet("Report"));
  std::vector<grparse::Warning> warnings;
  grparse::adopt_calamine_cells(calamine.document(), {}, &libre.document(), &warnings);
  const docv1::TableData& data = libre.document().tables(0).data();
  const docv1::TableCell* title = at(data, 0, 0);
  require_equal(title->col_span(), 3, "the title keeps libreoffice's span");
  require_equal(title->end_col_offset_idx(), 3, "and its end offset");
  require(title->row_section(), "a lone merged title is a section row");
  for (int column = 0; column < 3; ++column) {
    require(at(data, 1, column)->column_header(), "the header line is marked");
  }
  require(!at(data, 2, 0)->column_header(), "records are not header");
}

void verify_calamine_merges_win() {
  Workbook libre("libreoffice");
  docv1::TableData* libre_data = libre.sheet("S");
  text(libre_data, 0, 0, "Group", 2);
  text(libre_data, 1, 0, "a");
  text(libre_data, 1, 1, "b");
  number(libre_data, 2, 0, 1, "1");
  number(libre_data, 2, 1, 2, "2");
  Workbook calamine("calamine");
  docv1::TableData* calamine_data = calamine.sheet("S");
  text(calamine_data, 0, 0, "Group");
  text(calamine_data, 1, 0, "a");
  text(calamine_data, 1, 1, "b");
  number(calamine_data, 2, 0, 1, "1");
  number(calamine_data, 2, 1, 2, "2");
  // calamine read a different merge: the label over rows 0 and 1 of column 0.
  grparse::apply_merged_areas(calamine_data, {{0, 0, 2, 1}});
  std::vector<grparse::Warning> warnings;
  grparse::adopt_calamine_cells(calamine.document(), {}, &libre.document(), &warnings);
  const docv1::TableData& data = libre.document().tables(0).data();
  require_equal(at(data, 0, 0)->row_span(), 2, "calamine's row span");
  require_equal(at(data, 0, 0)->col_span(), 1, "libreoffice's column span is not applied");
  require(at(data, 1, 0) == nullptr, "the covered cell is dropped");
  require(at(data, 1, 1) != nullptr, "a cell outside the area stays");
}

void verify_apply_merged_areas() {
  docv1::TableData data;
  text(&data, 0, 0, "Title");
  text(&data, 0, 1, "");
  text(&data, 2, 0, "x");
  grparse::apply_merged_areas(&data, {{0, 0, 1, 2}, {1, 0, 2, 2}});
  require_equal(data.table_cells_size(), 2, "the covered cell of the first area is dropped");
  require_equal(at(data, 0, 0)->col_span(), 2, "the title spans two columns");
  require(at(data, 1, 0) == nullptr, "a blank merge places nothing");
  require_equal(at(data, 2, 0)->row_span(), 1, "a cell below a blank merge keeps its span");
}

void verify_sheet_without_table_is_named() {
  Workbook libre("libreoffice");
  report_libre(libre.sheet("Report"));
  Workbook calamine("calamine");
  report_calamine(calamine.sheet("Report"));
  text(calamine.sheet("Hidden"), 0, 0, "x");
  std::vector<grparse::Warning> warnings;
  const int adopted =
      grparse::adopt_calamine_cells(calamine.document(), {}, &libre.document(), &warnings);
  require_equal(adopted, 1, "only the matched sheet is adopted");
  require_equal(warnings.size(), size_t{1}, "the unmatched sheet is named");
  require(warnings[0].message.contains("'Hidden'"), "by its name");
}

// A sheet calamine did not read to its end is not adopted, whether a part
// of it arrived or none did: the table keeps every cell it had, and the
// warning that says so names the table by ref, the reader whose cells stand
// and the reason, under the code a consumer acts on.
void verify_incomplete_sheet_keeps_the_first_reader_cells() {
  Workbook libre("libreoffice");
  report_libre(libre.sheet("Report"));
  report_libre(libre.sheet("Unread"));
  text(libre.sheet("Whole"), 0, 0, "as libreoffice read it");
  Workbook calamine("calamine");
  // The part of Report that arrived before its read stopped.
  text(calamine.sheet("Report"), 0, 0, "Expenditure over threshold");
  text(calamine.sheet("Whole"), 0, 0, "as calamine read it");
  const docv1::Document before = libre.document();
  std::vector<grparse::Warning> warnings;
  const int adopted = grparse::adopt_calamine_cells(
      calamine.document(),
      {{"Report", "cell table corrupt"}, {"Unread", "worker died"}, {"Absent", "worker died"}},
      &libre.document(), &warnings);
  require_equal(adopted, 1, "only the sheet read to its end is adopted");
  using google::protobuf::util::MessageDifferencer;
  require(MessageDifferencer::Equals(libre.document().tables(0), before.tables(0)),
          "a sheet calamine read in part is left exactly as libreoffice read it");
  require(MessageDifferencer::Equals(libre.document().tables(1), before.tables(1)),
          "a sheet calamine never reached is left exactly as libreoffice read it");
  require_equal(at(libre.document().tables(2).data(), 0, 0)->text(),
                std::string("as calamine read it"), "the whole sheet's text");
  require_equal(warnings.size(), size_t{2},
                "one warning per sheet held back; a sheet with no table has none to keep");
  for (size_t index = 0; index < warnings.size(); ++index) {
    require(warnings[index].code == docv1::WARNING_CODE_SHEET_CELLS_NOT_ADOPTED,
            "the warning states its code");
    require_equal(warnings[index].item_ref, "#/tables/" + std::to_string(index),
                  "the table the warning is about");
    require(warnings[index].message.contains("libreoffice's cells"),
            "the words name the reader whose cells stand");
  }
  require(warnings[0].message.contains("'Report'") &&
              warnings[0].message.contains("cell table corrupt"),
          "the words name the sheet and the reason: " + warnings[0].message);
}

// The coordinator hands the leg's own account of what it did not finish to
// the adoption, and the typed warning comes back attributed to calamine.
void verify_coordinator_holds_back_an_incomplete_sheet() {
  std::vector<grparse::PlannedCollector> plan;
  plan.push_back({parsev1::COLLECTOR_LIBREOFFICE, [] {
                    grparse::CollectorOutcome outcome;
                    Workbook libre("libreoffice");
                    report_libre(libre.sheet("Report"));
                    outcome.document = libre.document();
                    outcome.success = true;
                    return outcome;
                  }});
  plan.push_back({parsev1::COLLECTOR_CALAMINE, [] {
                    grparse::CollectorOutcome outcome;
                    Workbook calamine("calamine");
                    text(calamine.sheet("Report"), 0, 0, "Expenditure over threshold");
                    outcome.document = calamine.document();
                    outcome.incomplete_sheets.push_back({"Report", "worker died"});
                    outcome.success = true;
                    return outcome;
                  }, true});
  const grparse::CoordinatorResult result = grparse::run_collectors(std::move(plan), {});
  require_equal(result.succeeded, 2, "both legs merged");
  require_equal(result.document.tables(0).data().table_cells_size(), 10,
                "every cell libreoffice read is still there");
  require_equal(result.warnings.size(), size_t{1}, "the held-back sheet is one warning");
  require(result.warnings[0].first == parsev1::COLLECTOR_CALAMINE &&
              result.warnings[0].second.code == docv1::WARNING_CODE_SHEET_CELLS_NOT_ADOPTED &&
              result.warnings[0].second.item_ref == result.document.tables(0).self_ref(),
          "calamine's warning names the table by code and ref");
}

void verify_coordinator_adopts_calamine_cells() {
  std::vector<grparse::PlannedCollector> plan;
  plan.push_back({parsev1::COLLECTOR_LIBREOFFICE, [] {
                    grparse::CollectorOutcome outcome;
                    Workbook libre("libreoffice");
                    report_libre(libre.sheet("Report"));
                    outcome.document = libre.document();
                    outcome.success = true;
                    return outcome;
                  }});
  plan.push_back({parsev1::COLLECTOR_CALAMINE, [] {
                    grparse::CollectorOutcome outcome;
                    Workbook calamine("calamine");
                    report_calamine(calamine.sheet("Report"));
                    outcome.document = calamine.document();
                    outcome.success = true;
                    return outcome;
                  }, true});
  const grparse::CoordinatorResult result = grparse::run_collectors(std::move(plan), {});
  require_equal(result.succeeded, 2, "both legs merged");
  require_equal(result.document.tables_size(), 1, "one table, not a second copy");
  require_equal(at(result.document.tables(0).data(), 2, 2)->text(), std::string("3495.00"),
                "the merged table retains the source numeric display");
}

void verify_calamine_alone_is_the_body() {
  std::vector<grparse::PlannedCollector> plan;
  plan.push_back({parsev1::COLLECTOR_LIBREOFFICE, [] {
                    grparse::CollectorOutcome outcome;
                    outcome.error = "libreoffice down";
                    return outcome;
                  }});
  plan.push_back({parsev1::COLLECTOR_CALAMINE, [] {
                    grparse::CollectorOutcome outcome;
                    Workbook calamine("calamine");
                    report_calamine(calamine.sheet("Report"));
                    outcome.document = calamine.document();
                    outcome.success = true;
                    return outcome;
                  }, true});
  const grparse::CoordinatorResult result = grparse::run_collectors(std::move(plan), {});
  require_equal(result.succeeded, 1, "calamine merged");
  require_equal(result.document.tables_size(), 1, "its table is the body");
}

void verify_formatted_values_and_formula_caches() {
  Workbook libre("libreoffice");
  auto* left = libre.sheet("S");
  number(left, 0, 0, 0.125, "12.5%")->mutable_value()->set_number_format("0.0%");
  number(left, 0, 1, 1234.5, "$1,234.50")->mutable_value()->set_number_format("$#,##0.00");
  number(left, 0, 2, 9, "9.00")->mutable_value()->set_number_format("0.00");
  text(left, 1, 0, "3")->mutable_value()->set_formula("SUM(A1:A2)");
  // A formula as the libreoffice fold shapes it when the worker states the
  // result: the cached number typed, the source beside it.
  auto* typed = number(left, 1, 2, 1234.5, "$1,234.50");
  typed->mutable_value()->set_number_format("$#,##0.00");
  typed->mutable_value()->set_formula_source("SUM(B1:B1)");
  Workbook calamine("calamine");
  auto* right = calamine.sheet("S");
  number(right, 0, 0, 0.125, "0.125");
  number(right, 0, 1, 1234.5, "1234.5");
  number(right, 0, 2, 10, "10");
  auto* formula = number(right, 1, 0, 3, "3");
  formula->mutable_value()->set_formula_source("SUM(A1:A2)");
  auto* only = number(right, 1, 1, 42, "42");
  only->mutable_value()->set_formula_source("6*7");
  number(right, 1, 2, 1234.5, "1234.5")->mutable_value()->set_formula_source("SUM(B1:B1)");
  std::vector<grparse::Warning> warnings;
  grparse::adopt_calamine_cells(calamine.document(), {}, &libre.document(), &warnings);
  const auto& result = libre.document().tables(0).data();
  require_equal(at(result, 0, 0)->text(), std::string("12.5%"), "percent display retained");
  require_equal(at(result, 0, 1)->text(), std::string("$1,234.50"), "currency display retained");
  require_equal(at(result, 0, 2)->text(), std::string("10"), "different value uses calamine text");
  require_equal(at(result, 1, 0)->value().number(), 3.0, "formula cached value retained");
  require_equal(at(result, 1, 0)->value().formula_source(), std::string("SUM(A1:A2)"),
                "formula and cache coexist");
  require_equal(at(result, 1, 1)->value().formula_source(), std::string("6*7"),
                "calamine-only formula retained");
  require_equal(at(result, 1, 2)->text(), std::string("$1,234.50"),
                "a typed formula result keeps its display like any formatted number");
  require_equal(at(result, 1, 2)->value().number(), 1234.5, "typed formula value retained");
  require_equal(at(result, 1, 2)->value().formula_source(), std::string("SUM(B1:B1)"),
                "typed formula source retained");
}

// A formula the file stores with no cached result (a workbook a library
// wrote and no spreadsheet program ever saved): calamine reads the source
// and nothing else, libreoffice computed the result when it loaded the file.
// calamine's empty reading is not a stored value to prefer, so the computed
// text and value stand, in both shapes the libreoffice fold gives a formula.
void verify_uncached_formula_keeps_libreoffice_result() {
  Workbook libre("libreoffice");
  auto* left = libre.sheet("S");
  number(left, 0, 0, 1, "1");
  number(left, 1, 0, 2, "2");
  number(left, 2, 0, 3, "3")->mutable_value()->set_formula_source("SUM(A1:A2)");
  text(left, 2, 1, "3")->mutable_value()->set_formula("SUM(A1:A2)");
  Workbook calamine("calamine");
  auto* right = calamine.sheet("S");
  number(right, 0, 0, 1, "1");
  number(right, 1, 0, 2, "2");
  text(right, 2, 0, "")->mutable_value()->set_formula_source("SUM(A1:A2)");
  text(right, 2, 1, "")->mutable_value()->set_formula_source("SUM(A1:A2)");
  std::vector<grparse::Warning> warnings;
  grparse::adopt_calamine_cells(calamine.document(), {}, &libre.document(), &warnings);
  const auto& result = libre.document().tables(0).data();
  require_equal(at(result, 2, 0)->text(), std::string("3"),
                "the computed text of a formula calamine has no result for");
  require_equal(at(result, 2, 0)->value().number(), 3.0, "its computed value");
  require_equal(at(result, 2, 0)->value().formula_source(), std::string("SUM(A1:A2)"),
                "its source");
  require_equal(at(result, 2, 1)->text(), std::string("3"),
                "the computed text of a formula on the legacy arm");
  require_equal(at(result, 2, 1)->value().formula_source(), std::string("SUM(A1:A2)"),
                "its source, beside the legacy arm");
}

void verify_declared_headers_survive_adoption() {
  Workbook libre("libreoffice");
  auto* left = libre.sheet("S");
  text(left, 0, 0, "A"); text(left, 0, 1, "B");
  number(left, 1, 0, 1, "1"); number(left, 1, 1, 2, "2");
  Workbook calamine("calamine");
  auto* right = calamine.sheet("S");
  *right = *left;
  auto* table = right->add_declared_tables();
  table->set_name("Declared");
  table->mutable_range()->mutable_start()->set_row(0);
  table->mutable_range()->mutable_start()->set_col(0);
  table->mutable_range()->mutable_end()->set_row(1);
  table->mutable_range()->mutable_end()->set_col(1);
  table->set_header_row_count(1);
  table->add_columns("Alpha"); table->add_columns("Beta");
  std::vector<grparse::Warning> warnings;
  grparse::adopt_calamine_cells(calamine.document(), {}, &libre.document(), &warnings);
  const auto& result = libre.document().tables(0).data();
  require(result.declared_tables_size() == 1 && result.columns(1).name() == "Beta",
          "table declarations and named columns survive adoption");
  require(at(result, 0, 0)->column_header() && !at(result, 1, 0)->column_header(),
          "declared range controls headers");
  // Explicitly headerless declarations must suppress inference and the
  // primary's old header flags, including on small all-text tables.
  right->mutable_declared_tables(0)->set_header_row_count(0);
  grparse::adopt_calamine_cells(calamine.document(), {}, &libre.document(), &warnings);
  require(!at(libre.document().tables(0).data(), 0, 0)->column_header(),
          "explicit zero headers is not overwritten by inference");
}

// A workbook embedded in a sheet folds into sheet groups of its own, under
// the embedded object's group. One of them can share a sheet's name
// ("Sheet1" inside "Sheet1" is the common case); calamine reads the
// workbook's own sheets, so its cells go to the sheet under the body and
// never to the embedded one.
void verify_an_embedded_workbooks_sheet_is_not_adopted_into() {
  Workbook libre("libreoffice");
  docv1::Document& document = libre.document();
  // The embedded workbook streams ahead of the sheets, so its groups come
  // first in the arena.
  docv1::GroupItem* book = document.add_groups();
  book->set_self_ref("#/groups/0");
  book->mutable_parent()->set_ref("#/body");
  book->set_name("Object 1");
  document.mutable_body()->add_children()->set_ref(book->self_ref());
  docv1::GroupItem* inner = document.add_groups();
  inner->set_self_ref("#/groups/1");
  inner->mutable_parent()->set_ref(book->self_ref());
  inner->set_name("Report");
  inner->set_label(docv1::GROUP_LABEL_SHEET);
  document.mutable_groups(0)->add_children()->set_ref(inner->self_ref());
  docv1::TableItem* inner_table = document.add_tables();
  inner_table->set_self_ref("#/tables/0");
  inner_table->mutable_parent()->set_ref("#/groups/1");
  inner_table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  document.mutable_groups(1)->add_children()->set_ref(inner_table->self_ref());
  text(inner_table->mutable_data(), 0, 0, "embedded cell");
  report_libre(libre.sheet("Report"));

  Workbook calamine("calamine");
  report_calamine(calamine.sheet("Report"));
  std::vector<std::string> warnings;
  const int adopted =
      grparse::adopt_calamine_cells(calamine.document(), {}, &libre.document(), &warnings);
  require_equal(adopted, 1, "the sheet under the body is adopted");
  require_equal(document.tables(0).data().table_cells_size(), 1,
                "the embedded workbook's table keeps its own cells");
  require_equal(document.tables(0).data().table_cells(0).text(), std::string("embedded cell"),
                "and its own text");
  require_equal(at(document.tables(1).data(), 2, 1)->text(), std::string("1-31-16"),
                "the real sheet takes calamine's reading");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("workbook_cells_test", "ok", {
      verify_values_and_text_are_calamine,
      verify_formatted_values_and_formula_caches,
      verify_declared_headers_survive_adoption,
      verify_uncached_formula_keeps_libreoffice_result,
      verify_libre_merges_fill_in_for_calamine,
      verify_calamine_merges_win,
      verify_apply_merged_areas,
      verify_sheet_without_table_is_named,
      verify_incomplete_sheet_keeps_the_first_reader_cells,
      verify_coordinator_holds_back_an_incomplete_sheet,
      verify_an_embedded_workbooks_sheet_is_not_adopted_into,
      verify_coordinator_adopts_calamine_cells,
      verify_calamine_alone_is_the_body,
  });
}
