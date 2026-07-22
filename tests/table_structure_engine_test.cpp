#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "grparse/table_structure_engine.h"

namespace {

namespace fs = std::filesystem;

constexpr int kSkipExitCode = 77;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void verify_missing_model_fails_loudly() {
  bool threw = false;
  try {
    grparse::TableStructureEngine engine("/nonexistent/slanet.onnx");
  } catch (const std::exception&) {
    threw = true;
  }
  require(threw, "a missing structure model must throw at construction");
}

// The token grid placement is pure logic and must hold without a model.
void verify_token_parsing() {
  const auto box = [](int index) {
    return grparse::AxisAlignedBox{index * 10, index * 10, index * 10 + 5, index * 10 + 5};
  };

  // Header row with a colspan, body rows with a rowspan carried down.
  const std::vector<std::string> tokens = {
      "<thead>", "<tr>", "<td></td>", "<td", " colspan=\"2\"", ">", "</td>", "</tr>", "</thead>",
      "<tbody>", "<tr>", "<td", " rowspan=\"2\"", ">", "</td>", "<td></td>", "<td></td>", "</tr>",
      "<tr>", "<td></td>", "<td></td>", "</tr>", "</tbody>",
  };
  const std::vector<grparse::AxisAlignedBox> boxes = {box(0), box(1), box(2), box(3),
                                                      box(4), box(5), box(6)};
  const auto grid = grparse::structure_from_tokens(tokens, boxes);
  require(grid.rows == 3 && grid.cols == 3, "span table shape");
  require(grid.cells.size() == 7, "each td is one cell");
  require(grid.cells[0].row == 0 && grid.cells[0].col == 0 && grid.cells[0].header,
          "thead cells carry the header flag");
  require(grid.cells[1].col == 1 && grid.cells[1].col_span == 2 && grid.cells[1].header,
          "colspan cell placement");
  require(grid.cells[2].row == 1 && grid.cells[2].col == 0 && grid.cells[2].row_span == 2 &&
              !grid.cells[2].header,
          "rowspan cell placement outside thead");
  require(grid.cells[5].row == 2 && grid.cells[5].col == 1,
          "rowspan occupancy pushes the next row's first cell right");
  require(grid.cells[0].left == 0 && grid.cells[6].left == 60,
          "cell boxes follow td emission order");

  // Degenerate streams must not crash and produce empty grids.
  const auto empty = grparse::structure_from_tokens({}, {});
  require(empty.rows == 0 && empty.cols == 0 && empty.cells.empty(), "empty token stream");
  const auto no_boxes =
      grparse::structure_from_tokens({"<tr>", "<td></td>", "</tr>"}, {});
  require(no_boxes.cells.size() == 1 && no_boxes.cells[0].right == 0,
          "missing boxes leave zero geometry, not a crash");
}

// Reference output of RapidTable's PPTableStructurer (slanet-plus) on
// tests/data/span_table.png: one plain header cell, one colspan="2" header
// cell, then three 3-cell body rows; eleven cells, mean token score 0.9999.
// Regenerate with the scripts described in models/README.md if the model
// file is ever updated.
struct ExpectedCell {
  int row, col, row_span, col_span;
  float left, top, right, bottom;
};
const ExpectedCell kSpanTableGolden[] = {
    {0, 0, 1, 1, 17.1F, 29.8F, 275.2F, 85.3F},   {0, 1, 1, 2, 262.4F, 11.9F, 690.4F, 79.0F},
    {1, 0, 1, 1, 9.6F, 83.6F, 244.3F, 149.7F},   {1, 1, 1, 1, 249.3F, 85.6F, 477.8F, 147.6F},
    {1, 2, 1, 1, 459.3F, 84.5F, 709.9F, 145.9F}, {2, 0, 1, 1, 7.4F, 148.7F, 251.9F, 211.1F},
    {2, 1, 1, 1, 246.8F, 148.1F, 482.8F, 208.2F}, {2, 2, 1, 1, 467.0F, 146.6F, 710.8F, 204.6F},
    {3, 0, 1, 1, 13.4F, 211.9F, 251.1F, 280.0F}, {3, 1, 1, 1, 245.9F, 209.5F, 480.0F, 277.9F},
    {3, 2, 1, 1, 465.7F, 208.9F, 710.8F, 272.4F},
};

float iou(const grparse::StructuredCell& cell, const ExpectedCell& expected) {
  const float left = std::max(static_cast<float>(cell.left), expected.left);
  const float top = std::max(static_cast<float>(cell.top), expected.top);
  const float right = std::min(static_cast<float>(cell.right), expected.right);
  const float bottom = std::min(static_cast<float>(cell.bottom), expected.bottom);
  const float intersection = std::max(0.0F, right - left) * std::max(0.0F, bottom - top);
  const float area_cell = static_cast<float>(cell.right - cell.left) *
                          static_cast<float>(cell.bottom - cell.top);
  const float area_expected =
      (expected.right - expected.left) * (expected.bottom - expected.top);
  return intersection / (area_cell + area_expected - intersection);
}

void verify_span_table_matches_reference(const fs::path& model, const fs::path& image_path) {
  grparse::TableStructureEnginePool pool(model, 2);
  const cv::Mat image = cv::imread(image_path.string(), cv::IMREAD_COLOR);
  require(!image.empty(), "test image must load: " + image_path.string());

  const auto structure = pool.recognize(image);
  require(structure.rows == 4 && structure.cols == 3,
          "grid shape diverged from the reference (got " + std::to_string(structure.rows) + "x" +
              std::to_string(structure.cols) + ")");
  require(structure.cells.size() == std::size(kSpanTableGolden),
          "cell count diverged from the reference (got " + std::to_string(structure.cells.size()) +
              ")");
  require(structure.score > 0.99F, "reference run scores 0.9999 on this table");

  for (const auto& expected : kSpanTableGolden) {
    bool matched = false;
    for (const auto& cell : structure.cells) {
      if (cell.row == expected.row && cell.col == expected.col &&
          cell.row_span == expected.row_span && cell.col_span == expected.col_span &&
          iou(cell, expected) > 0.9F) {
        matched = true;
        break;
      }
    }
    require(matched, "no cell matched reference grid position (" + std::to_string(expected.row) +
                         "," + std::to_string(expected.col) + ")");
  }

  // Warm reuse: a second call must not rebuild sessions.
  const auto again = pool.recognize(image);
  require(again.cells.size() == structure.cells.size(), "repeat recognition must be deterministic");
  require(pool.stats().acquires >= 2 && pool.stats().discards == 0,
          "pool must lease warm sessions without discards");
}

void verify_rejects_empty_image(const fs::path& model) {
  grparse::TableStructureEngine engine(model);
  bool threw = false;
  try {
    engine.recognize(cv::Mat());
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "an empty crop must be rejected");
}

}  // namespace

int main() {
  try {
    verify_missing_model_fails_loudly();
    verify_token_parsing();

    const char* models_dir = std::getenv("GRPARSE_TEST_MODELS_DIR");
    const char* data_dir = std::getenv("GRPARSE_TEST_DATA_DIR");
    const fs::path model =
        fs::path(models_dir == nullptr ? "models" : models_dir) / "slanet_plus.onnx";
    const fs::path image =
        fs::path(data_dir == nullptr ? "tests/data" : data_dir) / "span_table.png";
    if (!fs::exists(model)) {
      std::cerr << "table-structure-engine-test: skipped, model not present: " << model << '\n';
      return kSkipExitCode;
    }
    require(fs::exists(image), "test image missing: " + image.string());

    verify_rejects_empty_image(model);
    verify_span_table_matches_reference(model, image);
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "table-structure-engine-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
