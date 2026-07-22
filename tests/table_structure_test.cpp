#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "grparse/region_geometry.h"
#include "grparse/table_structure.h"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

grparse::OcrLine line_at(std::string text, int left, int top, int width = 100, int height = 20) {
  return grparse::OcrLine{std::move(text),
                          {{left, top},
                           {left + width, top},
                           {left + width, top + height},
                           {left, top + height}},
                          std::nullopt};
}

const grparse::TableCellGeometry& cell_at(const grparse::TableGrid& grid, int row, int col) {
  return grid.cells[static_cast<size_t>(row) * grid.cols + col];
}

std::string cell_text(const grparse::OcrPage& page, const grparse::TableCellGeometry& cell) {
  std::string text;
  for (const size_t index : cell.line_indices) {
    if (!text.empty()) text.push_back(' ');
    text += page.lines[index].text;
  }
  return text;
}

// A 3x3 grid with 80px gutters must come back as exactly 3 rows and 3
// columns with every text in its own cell, regardless of input order.
void verify_simple_grid() {
  grparse::OcrPage page{1000, 600, {}};
  const grparse::LayoutRegion table{"table", 0.9F, 40, 40, 700, 250};
  page.regions = {table};
  page.lines = {
      line_at("C2", 450, 110), line_at("A1", 50, 50),  line_at("B3", 250, 170),
      line_at("A2", 50, 110),  line_at("C1", 450, 50), line_at("B1", 250, 50),
      line_at("C3", 450, 170), line_at("A3", 50, 170), line_at("B2", 250, 110),
  };
  const auto grid = grparse::build_table_grid(page, page.regions[0]);
  require(grid.rows == 3 && grid.cols == 3, "3x3 table must give a 3x3 grid");
  require(grid.cells.size() == 9, "grid covers every position");
  const std::vector<std::vector<std::string>> expected = {
      {"A1", "B1", "C1"}, {"A2", "B2", "C2"}, {"A3", "B3", "C3"}};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      const auto& cell = cell_at(grid, row, col);
      require(cell.row == row && cell.col == col, "cell coordinates are row-major");
      require(cell_text(page, cell) == expected[row][col],
              "cell (" + std::to_string(row) + "," + std::to_string(col) + ") text");
    }
  }
}

// Word spacing inside a cell is narrower than the gutter threshold (half a
// line height) and must not split into an extra column, while a blank grid
// position stays present and empty.
void verify_word_spacing_and_blank_cells() {
  grparse::OcrPage page{1000, 600, {}};
  page.regions = {{"table", 0.9F, 40, 40, 700, 250}};
  page.lines = {
      // Row 0, column 0 holds two boxes 6px apart (threshold is 10).
      line_at("Total", 50, 50, 60), line_at("due", 116, 50, 40),
      line_at("Amount", 300, 50),
      line_at("Rent", 50, 110),
      // Row 1, column 1 is intentionally blank.
      line_at("Sum", 50, 170), line_at("90", 300, 170, 40),
  };
  const auto grid = grparse::build_table_grid(page, page.regions[0]);
  require(grid.rows == 3 && grid.cols == 2, "word gaps must not create columns");
  require(cell_text(page, cell_at(grid, 0, 0)) == "Total due", "words join left to right");
  require(cell_at(grid, 1, 1).line_indices.empty(), "blank position stays empty");
  require(cell_text(page, cell_at(grid, 2, 1)) == "90", "short cells bind by overlap");
  const auto& merged = cell_at(grid, 0, 0);
  require(merged.box.left == 50 && merged.box.right == 156, "cell box unions member lines");
}

// Lines outside the region, or bound to a higher-confidence overlapping
// region, must not enter the grid; a textless table gives an empty grid.
void verify_line_binding() {
  grparse::OcrPage page{1000, 600, {}};
  page.regions = {
      {"table", 0.6F, 40, 40, 700, 250},
      {"text", 0.9F, 40, 40, 700, 80},  // overlaps the table's first band
  };
  page.lines = {
      line_at("caption", 50, 300),  // below the table entirely
      line_at("para", 50, 50),      // center inside both; text region wins on confidence
      line_at("data", 50, 170),
  };
  const auto grid = grparse::build_table_grid(page, page.regions[0]);
  require(grid.rows == 1 && grid.cols == 1, "only bound lines enter the grid");
  require(cell_text(page, cell_at(grid, 0, 0)) == "data", "table keeps its own lines");

  const grparse::OcrPage empty_page{1000, 600, {}};
  const grparse::LayoutRegion lonely{"table", 0.9F, 0, 0, 100, 100};
  const auto empty = grparse::build_table_grid(empty_page, lonely);
  require(empty.rows == 0 && empty.cols == 0 && empty.cells.empty(),
          "textless table yields an empty grid");
}

// Two tables on one page each build their own grid from their own lines.
void verify_multi_table_pages() {
  grparse::OcrPage page{1000, 900, {}};
  page.regions = {
      {"table", 0.9F, 40, 40, 700, 150},
      {"table", 0.9F, 40, 400, 700, 510},
  };
  page.lines = {
      line_at("top-left", 50, 50), line_at("top-right", 300, 50),
      line_at("bottom-only", 50, 410),
  };
  const auto first = grparse::build_table_grid(page, page.regions[0]);
  const auto second = grparse::build_table_grid(page, page.regions[1]);
  require(first.rows == 1 && first.cols == 2, "first table shape");
  require(second.rows == 1 && second.cols == 1, "second table shape");
  require(cell_text(page, cell_at(second, 0, 0)) == "bottom-only", "tables do not share lines");
}

// D1: crops are clipped views of the raster, empty when fully outside.
void verify_region_crops() {
  cv::Mat raster(100, 200, CV_8UC3, cv::Scalar(7, 7, 7));

  const grparse::LayoutRegion inside{"table", 0.9F, 10, 20, 60, 70};
  cv::Mat crop = grparse::crop_region(raster, inside);
  require(crop.cols == 50 && crop.rows == 50, "inside crop keeps region size");
  crop.at<cv::Vec3b>(0, 0) = {1, 2, 3};
  require(raster.at<cv::Vec3b>(20, 10) == cv::Vec3b(1, 2, 3),
          "crop is a zero-copy view of the raster");

  const grparse::LayoutRegion overflow{"table", 0.9F, 150, 60, 400, 300};
  const cv::Mat clipped = grparse::crop_region(raster, overflow);
  require(clipped.cols == 50 && clipped.rows == 40, "overflowing crop clips to the raster");

  const grparse::LayoutRegion outside{"table", 0.9F, 300, 300, 400, 400};
  require(grparse::crop_region(raster, outside).empty(), "fully outside crop is empty");
  const grparse::LayoutRegion degenerate{"table", 0.9F, 50, 50, 50, 90};
  require(grparse::crop_region(raster, degenerate).empty(), "zero-width crop is empty");
}

// Same lines, same grid, ten times over.
void verify_determinism() {
  grparse::OcrPage page{1000, 600, {}};
  page.regions = {{"table", 0.9F, 40, 40, 700, 250}};
  page.lines = {
      line_at("b", 250, 53), line_at("a", 50, 50), line_at("d", 250, 111),
      line_at("c", 50, 108), line_at("e", 452, 51),
  };
  const auto first = grparse::build_table_grid(page, page.regions[0]);
  for (int round = 0; round < 10; ++round) {
    const auto again = grparse::build_table_grid(page, page.regions[0]);
    require(again.rows == first.rows && again.cols == first.cols, "stable shape");
    for (size_t index = 0; index < first.cells.size(); ++index) {
      require(again.cells[index].line_indices == first.cells[index].line_indices,
              "stable cell membership");
    }
  }
}

}  // namespace

int main() {
  try {
    verify_simple_grid();
    verify_word_spacing_and_blank_cells();
    verify_line_binding();
    verify_multi_table_pages();
    verify_region_crops();
    verify_determinism();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "table-structure-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
