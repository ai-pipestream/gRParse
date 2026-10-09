// Calamine is the primary reader of workbook cells: a routed workbook's
// sheet tables keep libreoffice's item (its place, its refs) and take
// calamine's cells. Each case builds the two collector documents by hand,
// the way the two folds shape them, and checks the merged result: values
// and text are calamine's, a merged area libreoffice saw survives when
// calamine reports none, calamine's own merges win when it has them, the
// header band is marked over the adopted cells, and a sheet with no table
// to land in is named, not dropped silently.

#include <print>
#include <string>
#include <vector>

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
  std::vector<std::string> warnings;
  const int adopted =
      grparse::adopt_calamine_cells(calamine.document(), &libre.document(), &warnings);
  require_equal(adopted, 1, "sheet tables adopted");
  require(warnings.empty(), "no warning for a sheet both read");
  const docv1::TableItem& table = libre.document().tables(0);
  const docv1::TableCell* reference = at(table.data(), 2, 1);
  require(reference != nullptr, "the reference cell is placed");
  require_equal(reference->text(), std::string("1-31-16"), "reference text");
  require(!reference->has_value(), "a text cell carries no date value");
  const docv1::TableCell* amount = at(table.data(), 2, 2);
  require_equal(amount->text(), std::string("3495"), "amount text is calamine's");
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
  std::vector<std::string> warnings;
  grparse::adopt_calamine_cells(calamine.document(), &libre.document(), &warnings);
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
  std::vector<std::string> warnings;
  grparse::adopt_calamine_cells(calamine.document(), &libre.document(), &warnings);
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
  std::vector<std::string> warnings;
  const int adopted =
      grparse::adopt_calamine_cells(calamine.document(), &libre.document(), &warnings);
  require_equal(adopted, 1, "only the matched sheet is adopted");
  require_equal(warnings.size(), size_t{1}, "the unmatched sheet is named");
  require(warnings[0].contains("'Hidden'"), "by its name");
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
  require_equal(at(result.document.tables(0).data(), 2, 2)->text(), std::string("3495"),
                "the merged table holds calamine's cells");
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

}  // namespace

int main() {
  return grparse_test::run_test_main("workbook_cells_test", "ok", {
      verify_values_and_text_are_calamine,
      verify_libre_merges_fill_in_for_calamine,
      verify_calamine_merges_win,
      verify_apply_merged_areas,
      verify_sheet_without_table_is_named,
      verify_coordinator_adopts_calamine_cells,
      verify_calamine_alone_is_the_body,
  });
}
