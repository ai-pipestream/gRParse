#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "grparse/reading_order.h"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

grparse::OcrLine line_at(std::string text, int left, int top, int width = 200, int height = 20) {
  return grparse::OcrLine{std::move(text),
                          {{left, top},
                           {left + width, top},
                           {left + width, top + height},
                           {left, top + height}},
                          std::nullopt};
}

std::vector<std::string> ordered_texts(const grparse::OcrPage& page) {
  std::vector<std::string> texts;
  for (const size_t index : grparse::reading_order(page)) {
    texts.push_back(page.lines[index].text);
  }
  return texts;
}

// Two full-height columns.  Pure y-sorting interleaves them (A1 B1 A2 B2 …);
// reading order must finish the left column before starting the right one.
void verify_two_columns_without_regions() {
  grparse::OcrPage page{1000, 800, {}};
  page.lines = {
      line_at("A1", 50, 100),  line_at("B1", 550, 100), line_at("A2", 50, 200),
      line_at("B2", 550, 200), line_at("A3", 50, 300),  line_at("B3", 550, 300),
  };
  const auto texts = ordered_texts(page);
  const std::vector<std::string> expected = {"A1", "A2", "A3", "B1", "B2", "B3"};
  require(texts == expected, "column order must beat pure y-sort even without regions");
}

// The same page with detected column regions must give the same answer, and
// lines bind to their region even when slightly outside a sibling's band.
void verify_two_columns_with_regions() {
  grparse::OcrPage page{1000, 800, {}};
  page.lines = {
      line_at("B1", 550, 100), line_at("A1", 50, 100),  line_at("B2", 550, 200),
      line_at("A2", 50, 200),  line_at("B3", 550, 300), line_at("A3", 50, 300),
  };
  page.regions = {
      {"text", 0.9F, 40, 90, 460, 330},
      {"text", 0.85F, 540, 90, 960, 330},
  };
  const auto texts = ordered_texts(page);
  const std::vector<std::string> expected = {"A1", "A2", "A3", "B1", "B2", "B3"};
  require(texts == expected, "region-based column order failed");
}

// Title band above two columns: the full-width unit forms its own horizontal
// band and reads first.
void verify_title_band_reads_before_columns() {
  grparse::OcrPage page{1000, 800, {}};
  page.lines = {
      line_at("B1", 550, 200), line_at("A1", 50, 200), line_at("Title", 200, 40, 600),
      line_at("A2", 50, 300),  line_at("B2", 550, 300),
  };
  const auto texts = ordered_texts(page);
  const std::vector<std::string> expected = {"Title", "A1", "A2", "B1", "B2"};
  require(texts == expected, "title band must precede both columns");
}

// A figure region with no text must not disturb the text order.
void verify_textless_regions_are_ignored() {
  grparse::OcrPage page{1000, 800, {}};
  page.lines = {line_at("first", 50, 100), line_at("second", 50, 200)};
  page.regions = {{"picture", 0.8F, 500, 100, 900, 700}};
  const auto texts = ordered_texts(page);
  const std::vector<std::string> expected = {"first", "second"};
  require(texts == expected, "textless regions must drop out of the order");
}

// Same detections, same order: run the messy case repeatedly.
void verify_determinism() {
  grparse::OcrPage page{1000, 800, {}};
  page.lines = {
      line_at("c", 400, 205), line_at("a", 50, 100),  line_at("d", 60, 400),
      line_at("b", 420, 100), line_at("e", 500, 395),
  };
  const auto first = ordered_texts(page);
  for (int round = 0; round < 10; ++round) {
    require(ordered_texts(page) == first, "reading order must be deterministic");
  }
}

// Degenerate inputs must not crash or drop lines.
void verify_degenerate_inputs() {
  grparse::OcrPage empty{100, 100, {}};
  require(grparse::reading_order(empty).empty(), "empty page yields empty order");

  grparse::OcrPage with_blank{100, 100, {}};
  with_blank.lines = {line_at("kept", 10, 10), grparse::OcrLine{"", {}, std::nullopt}};
  const auto order = grparse::reading_order(with_blank);
  require(order.size() == 1 && with_blank.lines[order[0]].text == "kept",
          "blank lines are excluded, real lines kept");
}

// The box-level cut behind the line order: a full-width title band, two
// columns beneath it, a figure spanning both columns below them with its
// caption, and a footnote band at the bottom read as a reader would.
void verify_box_cut_reads_bands_then_columns() {
  const std::vector<grparse::OrderBox> boxes = {
      {550, 200, 950, 220},  // B1
      {50, 200, 450, 220},   // A1
      {200, 40, 800, 80},    // title
      {50, 250, 450, 270},   // A2
      {550, 250, 950, 270},  // B2
      {100, 400, 900, 600},  // figure across both columns
      {300, 610, 700, 625},  // caption under the figure
      {50, 700, 450, 715},   // footnote band
  };
  const std::vector<size_t> expected = {2, 1, 3, 0, 4, 5, 6, 7};
  require(grparse::xy_cut_order(boxes) == expected,
          "boxes read title, left column, right column, figure, caption, footnote");
  require(grparse::xy_cut_order({}).empty(), "no boxes, no order");
  require(grparse::xy_cut_order({{0, 0, 1, 1}}) == std::vector<size_t>{0},
          "one box orders itself");
}

// Boxes that overlap in both axes have no gap to cut; they fall back to
// top-then-left order and keep input order on exact ties.
void verify_box_cut_fallback_is_stable() {
  const std::vector<grparse::OrderBox> boxes = {
      {10, 10, 100, 100}, {10, 10, 100, 100}, {5, 20, 90, 90}, {0, 10, 50, 60},
  };
  const std::vector<size_t> expected = {3, 0, 1, 2};
  require(grparse::xy_cut_order(boxes) == expected, "overlapping boxes sort by top, left, input");
}

// The item-level policy: a paragraph gap wider than the gutter no longer
// splits rows first, and a gap beside a short label is not a gutter.
void verify_box_cut_policy() {
  const std::vector<grparse::OrderBox> columns = {
      {50, 100, 380, 180},  {420, 100, 750, 180},  // row 1, gutter 40 wide
      {50, 240, 380, 320},  {420, 240, 750, 320},  // row 2, 60 below row 1
  };
  const grparse::CutPolicy items{.band_over_gutter = 2.0, .gutter_side_share = 0.5};
  require(grparse::xy_cut_order(columns) == std::vector<size_t>{0, 1, 2, 3},
          "the widest-gap rule reads the rows");
  require(grparse::xy_cut_order(columns, items) == std::vector<size_t>{0, 2, 1, 3},
          "the item rule reads the columns");

  const std::vector<grparse::OrderBox> labelled = {
      {150, 80, 400, 90},    // text above the panels
      {150, 100, 500, 150},  // a row of panels
      {110, 110, 135, 120},  // a short label left of the panels
      {150, 170, 500, 185},  // code line under the panels
  };
  const std::vector<size_t> order = grparse::xy_cut_order(labelled, items);
  require(order.size() == 4 && order[0] == 0 && order[3] == 3,
          "a short label beside one row does not make a column of the whole block: the text "
          "above still reads first and the code line last");
}

}  // namespace

int main() {
  try {
    verify_box_cut_reads_bands_then_columns();
    verify_box_cut_fallback_is_stable();
    verify_box_cut_policy();
    verify_two_columns_without_regions();
    verify_two_columns_with_regions();
    verify_title_band_reads_before_columns();
    verify_textless_regions_are_ignored();
    verify_determinism();
    verify_degenerate_inputs();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "reading-order-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
