#include <cstddef>
#include <string>
#include <vector>

#include "grparse/assembly_blocks.h"
#include "support/check.h"

namespace {

using grparse_test::require;

grparse::OcrLine line_at(std::string text, int left, int top, int width = 200, int height = 10) {
  return grparse::OcrLine{std::move(text),
                          {{left, top}, {left + width, top}, {left + width, top + height},
                           {left, top + height}},
                          0.9F};
}

// The texts of the blocks in order, the float's slot marked by its name.
std::vector<std::string> sequence(const grparse::OcrPage& page, const grparse::LayoutRegion& region) {
  const std::vector<grparse::TextBlock> blocks = grparse::build_text_blocks(page);
  const size_t anchor = grparse::region_anchor(page, blocks, region);
  std::vector<std::string> out;
  for (size_t index = 0; index <= blocks.size(); ++index) {
    if (index == anchor) out.push_back("<" + region.label + ">");
    if (index < blocks.size()) out.push_back(page.lines[blocks[index].lines.front()].text);
  }
  return out;
}

// Rule 1: a float that owns lines (a table's cells) is emitted before them.
void verify_float_precedes_its_own_lines() {
  grparse::OcrPage page{1000, 1000, {line_at("intro", 50, 100), line_at("cell", 60, 320, 100),
                                     line_at("after", 50, 600)}};
  page.regions = {{"table", 0.9F, 40, 300, 960, 420}};
  const std::vector<std::string> expected = {"intro", "<table>", "cell", "after"};
  require(sequence(page, page.regions[0]) == expected, "a table anchors before its own lines");
}

// Rule 2: a text-less float anchors before the first block of its own
// column that starts below it, not before a left-column line level with it.
void verify_float_with_text_beside_it_stays_in_its_column() {
  grparse::OcrPage page{1000, 1000,
                        {line_at("left one", 10, 100, 80), line_at("left two", 10, 400, 80),
                         line_at("right one", 510, 100, 80), line_at("right two", 510, 400, 80)}};
  page.regions = {{"picture", 0.8F, 500, 200, 1000, 350}};
  const std::vector<std::string> expected = {"left one", "left two", "right one", "<picture>",
                                             "right two"};
  require(sequence(page, page.regions[0]) == expected,
          "a float lands between its own column's lines");
}

// Rule 3: a corner logo above a title that never reaches its column, with
// nothing level with it, heads the page instead of closing it.
void verify_lonely_float_heads_its_band() {
  grparse::OcrPage page{1000, 1500, {line_at("EU Network Codes", 60, 300, 500, 40),
                                     line_at("Mike Thorne", 60, 650, 300, 20)}};
  page.regions = {{"picture", 0.8F, 760, 60, 960, 180}};
  const std::vector<std::string> expected = {"<picture>", "EU Network Codes", "Mike Thorne"};
  require(sequence(page, page.regions[0]) == expected,
          "a logo with no text beside it reads before the text below it");
}

// Rule 3 again, lower on the page: a signature image beside no text reads
// between the line above it and the dated line below it, whatever column
// each is in.
void verify_lonely_float_reads_between_the_lines_around_it() {
  grparse::OcrPage page{1000, 1500, {line_at("SO ORDERED.", 90, 700, 150, 20),
                                     line_at("DATE: August 5, 2016", 60, 1200, 300, 20)}};
  page.regions = {{"picture", 0.8F, 450, 800, 780, 950}};
  const std::vector<std::string> expected = {"SO ORDERED.", "<picture>", "DATE: August 5, 2016"};
  require(sequence(page, page.regions[0]) == expected,
          "a signature beside no text reads before the dated line below it");
}

// Rule 4: a float closing the right column of a two-column page, with the
// left column running beside it, stays last.
void verify_float_closing_a_column_stays_last() {
  grparse::OcrPage page{1000, 1000,
                        {line_at("left one", 10, 100, 80), line_at("left two", 10, 400, 80),
                         line_at("left three", 10, 700, 80), line_at("right one", 510, 100, 80),
                         line_at("right two", 510, 400, 80)}};
  page.regions = {{"picture", 0.8F, 500, 600, 1000, 900}};
  const std::vector<std::string> expected = {"left one", "left two", "left three", "right one",
                                             "right two", "<picture>"};
  require(sequence(page, page.regions[0]) == expected,
          "a float ending the right column follows that column");
}

// A running header level with a corner logo is not a column: furniture
// never holds a float back.
void verify_furniture_beside_a_float_is_not_a_column() {
  grparse::OcrPage page{1000, 1500, {line_at("Running header", 60, 80, 300, 20),
                                     line_at("Body", 60, 650, 300, 20)}};
  page.regions = {{"page_header", 0.8F, 40, 60, 400, 110},
                  {"picture", 0.8F, 760, 60, 960, 180}};
  const std::vector<std::string> expected = {"Running header", "<picture>", "Body"};
  require(sequence(page, page.regions[1]) == expected,
          "a logo level with the running header still heads the body");
}

void verify_page_without_blocks_anchors_at_end() {
  grparse::OcrPage page{1000, 1000, {}};
  page.regions = {{"picture", 0.8F, 100, 100, 900, 900}};
  require(grparse::build_text_blocks(page).empty(), "no lines, no blocks");
  require(grparse::region_anchor(page, {}, page.regions[0]) == 0, "an empty page anchors at 0");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("assembly-blocks-test", {
      verify_float_precedes_its_own_lines,
      verify_float_with_text_beside_it_stays_in_its_column,
      verify_lonely_float_heads_its_band,
      verify_lonely_float_reads_between_the_lines_around_it,
      verify_float_closing_a_column_stays_last,
      verify_furniture_beside_a_float_is_not_a_column,
      verify_page_without_blocks_anchors_at_end,
  });
}
