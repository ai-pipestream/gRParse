#include <cstdlib>
#include <iostream>
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
  page.regions = {{"figure", 0.8F, 500, 100, 900, 700}};
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

}  // namespace

int main() {
  try {
    verify_two_columns_without_regions();
    verify_two_columns_with_regions();
    verify_title_band_reads_before_columns();
    verify_textless_regions_are_ignored();
    verify_determinism();
    verify_degenerate_inputs();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "reading-order-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
