#include <string>
#include <utility>
#include <vector>

#include "grparse/table_structure.h"
#include "support/check.h"

namespace {

using grparse_test::require;

grparse::OcrLine line_at(std::string text, int left, int top, int width = 40, int height = 20) {
  return grparse::OcrLine{std::move(text),
                          {{left, top}, {left + width, top}, {left + width, top + height}, {left, top + height}},
                          std::nullopt};
}

// Headings Name/A/B/C over three rows of values. `drifted` rows get model
// cell boxes 60px right of their words, as SLANet's boxes come out on a
// dense table; the other rows' boxes fit.
grparse::OcrPage scores_table(const std::vector<bool>& drifted) {
  grparse::OcrPage page{1000, 1000, {}};
  grparse::LayoutRegion table{"table", 0.9F, 80, 80, 600, 300};
  const std::vector<std::pair<int, int>> fitted = {{90, 170}, {190, 260}, {290, 360}, {390, 460}};
  for (int col = 0; col < 4; ++col) {
    table.structured_cells.push_back({0, col, 1, 1, true, fitted[col].first, 95, fitted[col].second, 125});
  }
  page.lines = {line_at("Name", 100, 100), line_at("A", 210, 100, 20), line_at("B", 310, 100, 20),
                line_at("C", 410, 100, 20)};
  const std::vector<std::string> labels = {"x", "y", "z"};
  for (int row = 1; row <= 3; ++row) {
    const int top = 140 + 40 * (row - 1);
    for (int col = 0; col < 4; ++col) {
      const int shift = col > 0 && drifted[static_cast<size_t>(row - 1)] ? 60 : 0;
      table.structured_cells.push_back(
          {row, col, 1, 1, false, fitted[col].first + shift, top - 5, fitted[col].second + shift, top + 25});
    }
    page.lines.push_back(line_at(labels[static_cast<size_t>(row - 1)], 100, top, 20));
    page.lines.push_back(line_at("0." + std::to_string(row) + "1", 200, top));
    page.lines.push_back(line_at("0." + std::to_string(row) + "2", 300, top));
    page.lines.push_back(line_at("0." + std::to_string(row) + "3", 400, top));
  }
  page.regions = {table};
  return page;
}

// Cell index of row r, column c in scores_table.
int cell(int row, int col) { return row * 4 + col; }

void verify_a_drifted_row_rejoins_its_columns() {
  const grparse::OcrPage page = scores_table({true, false, false});
  const auto assignment = grparse::assign_table_lines(page, page.regions[0]);
  // Line order: 4 headings, then per row label, A, B, C.
  require(assignment.cell_of_line[5] == cell(1, 1), "row 1's A value stays in column A");
  require(assignment.cell_of_line[6] == cell(1, 2), "row 1's B value leaves column A for column B");
  require(assignment.cell_of_line[7] == cell(1, 3), "row 1's C value leaves column B for column C");
  require(assignment.cell_of_line[10] == cell(2, 2) && assignment.cell_of_line[15] == cell(3, 3),
          "rows whose boxes fit keep their cells");
  for (int heading = 0; heading < 4; ++heading) {
    require(assignment.cell_of_line[static_cast<size_t>(heading)] == cell(0, heading), "headings never move");
  }
}

void verify_a_column_the_model_kept_together_is_left_alone() {
  // Every row drifted the same way: each visual column sits in one model
  // column on every row, so nothing says the model split it.
  const grparse::OcrPage page = scores_table({true, true, true});
  const auto assignment = grparse::assign_table_lines(page, page.regions[0]);
  require(assignment.cell_of_line[6] == cell(1, 1) && assignment.cell_of_line[10] == cell(2, 1) &&
              assignment.cell_of_line[14] == cell(3, 1),
          "a band the model kept in one column stays there");
}

void verify_a_text_table_is_left_alone() {
  grparse::OcrPage page = scores_table({true, false, false});
  for (size_t index = 4; index < page.lines.size(); ++index) {
    if (index % 4 != 0) page.lines[index].text = "word";
  }
  const auto assignment = grparse::assign_table_lines(page, page.regions[0]);
  require(assignment.cell_of_line[6] == cell(1, 1), "a table that is not mostly numbers keeps the model's cells");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("table-column-anchor-test", {
      verify_a_drifted_row_rejoins_its_columns,
      verify_a_column_the_model_kept_together_is_left_alone,
      verify_a_text_table_is_left_alone,
  });
}
