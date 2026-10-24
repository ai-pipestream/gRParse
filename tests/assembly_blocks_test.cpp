#include <algorithm>
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
// does not hold a float back, and the logo heads the page (before the
// header block, which is not body content anyway).
void verify_furniture_beside_a_float_is_not_a_column() {
  grparse::OcrPage page{1000, 1500, {line_at("Running header", 60, 80, 300, 20),
                                     line_at("Body", 60, 650, 300, 20)}};
  page.regions = {{"page_header", 0.8F, 40, 60, 400, 110},
                  {"picture", 0.8F, 760, 60, 960, 180}};
  const std::vector<std::string> expected = {"<picture>", "Running header", "Body"};
  require(sequence(page, page.regions[1]) == expected,
          "a logo level with the running header still heads the body");
}

// Word-by-word recognition (0073 p5): the slide heading is three word
// lines in one region, only the last reaching the logo's column, and a
// second logo with its own text sits at the bottom right. The top logo
// anchors before the heading, not before the bottom logo's text.
void verify_float_column_uses_block_hulls() {
  grparse::OcrPage page{2339, 1653,
                        {line_at("Staff", 731, 281, 222, 112), line_at("Survey", 981, 281, 333, 112),
                         line_at("cont..", 1342, 281, 266, 112),
                         line_at("The Council receives better ratings", 339, 512, 1600, 100),
                         line_at("Simon Atkinson, Ipsos Mori", 1296, 855, 750, 50),
                         line_at("Hackney", 1640, 1460, 480, 80)}};
  page.regions = {{"section_header", 0.9F, 720, 270, 1620, 400},
                  {"text", 0.9F, 330, 500, 1950, 830},
                  {"picture", 0.96F, 1525, 73, 2172, 456},
                  {"picture", 0.97F, 1631, 1452, 2126, 1545}};
  const std::vector<std::string> expected = {"<picture>", "Staff", "The Council receives better ratings",
                                             "Simon Atkinson, Ipsos Mori", "Hackney"};
  require(sequence(page, page.regions[2]) == expected,
          "a corner logo over a word-split heading reads before the heading");
}

// A cover page (0047 p1): a photo top left, the publisher's mark text at the
// bottom right and a version line under it at the bottom left. Nothing on
// the page reads in columns, so the photo anchors before the mark's text,
// not before the version line that happens to share its horizontal span.
void verify_cover_photo_precedes_the_next_block_in_any_column() {
  grparse::OcrPage page{1654, 2360, {line_at("SECURE TENANCY", 130, 230, 800, 340),
                                     line_at("NORTHAMPTON BOROUGH COUNCIL", 1300, 2141, 300, 30),
                                     line_at("Version: June 2009", 180, 2253, 220, 25)}};
  page.regions = {{"picture", 0.9F, 130, 632, 1030, 1500},
                  {"picture", 0.9F, 1310, 1990, 1600, 2130}};
  const std::vector<std::string> expected = {"SECURE TENANCY", "<picture>",
                                             "NORTHAMPTON BOROUGH COUNCIL", "Version: June 2009"};
  require(sequence(page, page.regions[0]) == expected,
          "a cover photo reads before the first block under it, whatever its column");
}

void verify_page_without_blocks_anchors_at_end() {
  grparse::OcrPage page{1000, 1000, {}};
  page.regions = {{"picture", 0.8F, 100, 100, 900, 900}};
  require(grparse::build_text_blocks(page).empty(), "no lines, no blocks");
  require(grparse::region_anchor(page, {}, page.regions[0]) == 0, "an empty page anchors at 0");
}

// A text layer hands a list over as word cells: the words of one bullet
// line make one block, each bullet line its own, and a list two columns
// away on the same baseline stays apart.
void verify_list_words_on_one_line_make_one_item() {
  grparse::OcrPage page{1000, 1000,
                        {line_at("-", 50, 100, 6), line_at("One", 62, 100, 30),
                         line_at("page", 98, 100, 36), line_at("event", 140, 100, 40),
                         line_at("-", 50, 130, 6), line_at("Provenance", 62, 130, 80),
                         line_at("-", 600, 100, 6), line_at("Far", 612, 100, 30)}};
  page.regions = {{"list", 0.9F, 40, 90, 960, 145}};
  const std::vector<grparse::TextBlock> blocks = grparse::build_text_blocks(page);
  std::vector<std::vector<size_t>> lines;
  for (const auto& block : blocks) lines.push_back(block.lines);
  std::ranges::sort(lines);
  const std::vector<std::vector<size_t>> expected = {{0, 1, 2, 3}, {4, 5}, {6, 7}};
  require(lines == expected,
          "one block per bullet line; the far list on the same baseline is its own");
}

std::vector<std::vector<size_t>> sorted_block_lines(const grparse::OcrPage& page) {
  std::vector<std::vector<size_t>> lines;
  for (const auto& block : grparse::build_text_blocks(page)) lines.push_back(block.lines);
  std::ranges::sort(lines);
  return lines;
}

// Items written inline on one line, each opened by a marker like the
// line's first, stay separate items; a decimal, an initial or a dash in an
// item's text is no marker of its list.
void verify_inline_list_markers_split_items() {
  grparse::OcrPage page{1000, 1000,
                        {line_at("\u2022", 50, 100, 6), line_at("Red", 62, 100, 20),
                         line_at("\u2022", 88, 100, 6), line_at("Green", 100, 100, 30),
                         line_at("\u2022", 136, 100, 6), line_at("Blue", 148, 100, 25),
                         line_at("\u2610", 50, 130, 8), line_at("Yes", 62, 130, 20),
                         line_at("\u2610", 88, 130, 8), line_at("No", 100, 130, 15),
                         line_at("a)", 50, 160, 10), line_at("foo", 64, 160, 20),
                         line_at("b)", 90, 160, 10), line_at("bar", 104, 160, 20),
                         line_at("1.", 50, 190, 10), line_at("Size", 64, 190, 25),
                         line_at("1.5", 93, 190, 15), line_at("mm", 112, 190, 15),
                         line_at("\u2022", 50, 220, 6), line_at("J.", 60, 220, 10),
                         line_at("Smith", 74, 220, 30), line_at("-", 108, 220, 5),
                         line_at("warm", 117, 220, 25)}};
  page.regions = {{"list", 0.9F, 40, 90, 960, 240}};
  const std::vector<std::vector<size_t>> expected = {
      {0, 1}, {2, 3}, {4, 5}, {6, 7}, {8, 9}, {10, 11}, {12, 13}, {14, 15, 16, 17},
      {18, 19, 20, 21, 22}};
  require(sorted_block_lines(page) == expected,
          "each inline bullet, check box and lettered item is its own item, and an item's "
          "own decimal, initial and dash stay in its text");
}

// A bullet's hanging indent is wider than a line height (Word's default puts
// about 13pt between the glyph and a 12.6pt line), so the word after a lone
// marker joins it across two line heights; between words the reach stays one.
void verify_marker_reaches_across_its_hanging_indent() {
  grparse::OcrPage page{1000, 1000,
                        {line_at("\u2022", 50, 100, 6), line_at("Hanging", 74, 100, 50),
                         line_at("indent", 130, 100, 30), line_at("Text", 50, 130, 20),
                         line_at("apart", 88, 130, 30), line_at("\u2022", 50, 160, 6),
                         line_at("Far", 90, 160, 20)}};
  page.regions = {{"list", 0.9F, 40, 90, 960, 175}};
  const std::vector<std::vector<size_t>> expected = {{0, 1, 2}, {3}, {4}, {5}, {6}};
  require(sorted_block_lines(page) == expected,
          "a lone marker reaches its text across the hanging indent, words do not, and a "
          "marker's reach ends at two line heights");
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
      verify_float_column_uses_block_hulls,
      verify_cover_photo_precedes_the_next_block_in_any_column,
      verify_page_without_blocks_anchors_at_end,
      verify_list_words_on_one_line_make_one_item,
      verify_inline_list_markers_split_items,
      verify_marker_reaches_across_its_hanging_indent,
  });
}
