#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "grparse/table_structure_engine.h"
#include "grparse/gpu_selection.h"
#include "grparse_session_ep.h"

#ifdef GRPARSE_TEST_NATIVE_OPENVINO
#include <openvino/c/openvino.h>
#endif
#include "support/check.h"

namespace {

namespace fs = std::filesystem;

using grparse_test::require;

void verify_missing_model_fails_loudly() {
  bool threw = false;
  try {
    grparse::TableStructureEngine engine("/nonexistent/slanet.onnx");
  } catch (const std::exception&) {
    threw = true;
  }
  require(threw, "a missing structure model must throw at construction");
}

// Exercise actual constructor routing without depending on accelerator hardware.
// The deliberately invalid original model must fail on the selected ORT EP;
// only native GPU routes may request the derived model/vocabulary artifacts.
void verify_table_backend_routing() {
  const fs::path directory = fs::temp_directory_path() /
      ("grparse-table-artifacts-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    fs::path directory;
    grparse::OrtEpSelection selection;
    ~Cleanup() {
      grparse::set_ort_ep_selection(selection);
      std::error_code error;
      fs::remove_all(directory, error);
    }
  } cleanup{directory, grparse::ort_ep_selection()};
  require(fs::create_directory(directory), "create isolated table artifact test directory");
  const auto model = directory / "slanet_plus.onnx";
  std::ofstream(model) << "unparsed original model placeholder";
  grparse::OrtEpSelection npu;
  npu.ep = grparse::OrtEp::kOpenVino;
  npu.openvino_device = "NPU";
  grparse::set_ort_ep_selection(npu);
  const auto hooks_before = grparse::ep_hook_invocations();
  const auto fallbacks_before = grparse::ep_fallback_count();
  std::string npu_error;
  try {
    grparse::TableStructureEngine engine(model);
  } catch (const std::exception& failure) {
    npu_error = failure.what();
  }
  require(npu_error.find("gRParse table structure: OpenVINO failed to build") != std::string::npos &&
              npu_error.find("refusing CPU fallback") != std::string::npos,
          "NPU tables must use the strict ORT factory with the original model: " + npu_error);
  require(grparse::ep_hook_invocations() > hooks_before,
          "NPU table construction must reach the configured ORT provider");
  require(grparse::ep_fallback_count() == fallbacks_before,
          "a failed NPU table must never rebuild on CPU");
  require(grparse::ort_ep_selection().openvino_device == "NPU",
          "NPU table failures must preserve the requested device");
  std::println("table-structure-engine-test: NPU routes through strict ORT; invalid model rejected");
#ifdef GRPARSE_TEST_NATIVE_OPENVINO
  grparse::OrtEpSelection gpu;
  gpu.ep = grparse::OrtEp::kOpenVino;
  for (const char* device : {"GPU", "GPU.0", "GPU.12"}) {
    gpu.openvino_device = device;
    grparse::set_ort_ep_selection(gpu);
    for (const char* missing : {"slanet_plus_openvino.tokens", "slanet_plus_openvino.onnx"}) {
      std::string error;
      try {
        grparse::TableStructureEngine engine(model);
      } catch (const std::exception& failure) {
        error = failure.what();
      }
      require(error.find(missing) != std::string::npos,
              std::string(device) + " must use native table artifacts: " + missing + ": " + error);
      std::ofstream(directory / "slanet_plus_openvino.tokens") << "<td></td>\n";
    }
    fs::remove(directory / "slanet_plus_openvino.tokens");
  }
#endif
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

void verify_reference(const grparse::TableStructure& structure) {
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
}

void verify_span_table_matches_reference(const fs::path& model, const fs::path& image_path) {
  grparse::TableStructureEnginePool pool(model, 2);
  const cv::Mat image = cv::imread(image_path.string(), cv::IMREAD_COLOR);
  require(!image.empty(), "test image must load: " + image_path.string());
  const auto structure = pool.recognize(image);
  verify_reference(structure);

  // Reuse the pool: both calls must preserve the complete reference grid.
  const auto again = pool.recognize(image);
  verify_reference(again);
  require(again.cells.size() == structure.cells.size(), "repeat recognition must be deterministic");
  require(pool.stats().acquires >= 2 && pool.stats().discards == 0,
          "pool must lease warm sessions without discards");
}

void verify_b70(const fs::path& model, const fs::path& image_path) {
#ifdef GRPARSE_TEST_NATIVE_OPENVINO
  require(grparse::read_gpu_hardware().intel_b70, "B70 test requires the actual Intel B70 PCI device");
  ov_core_t* raw_core = nullptr;
  require(ov_core_create(&raw_core) == OK, "B70 test must create native OpenVINO core");
  const std::unique_ptr<ov_core_t, decltype(&ov_core_free)> core(raw_core, ov_core_free);
  char* raw_name = nullptr;
  require(ov_core_get_property(core.get(), "GPU.0", "FULL_DEVICE_NAME", &raw_name) == OK,
          "B70 test must identify GPU.0");
  const std::string name = raw_name == nullptr ? "" : raw_name;
  ov_free(raw_name);
  require(grparse::is_b70_openvino_device_name(name), "GPU.0 must identify B70: " + name);
  std::println("table-structure-engine-test: strict B70 GPU.0: {}", name);
  grparse::OrtEpSelection selection;
  selection.ep = grparse::OrtEp::kOpenVino;
  selection.openvino_device = "GPU.0";
  grparse::set_ort_ep_selection(selection);
  const auto fallbacks = grparse::ep_fallback_count();
  grparse::TableStructureEngine engine(model);
  const cv::Mat image = cv::imread(image_path.string(), cv::IMREAD_COLOR);
  require(!image.empty(), "B70 golden image must load");
  // Same engine is essential: a reused compiled Loop used to return only two
  // decoder steps on its second call. Both calls must match every golden cell.
  for (int call = 0; call < 2; ++call) {
    const auto structure = engine.recognize(image);
    verify_reference(structure);
    std::println("table-structure-engine-test: B70 call {}: {}x{}, {} cells, score {}",
                 call + 1, structure.rows, structure.cols, structure.cells.size(), structure.score);
  }
  require(grparse::ep_fallback_count() == fallbacks, "B70 table calls must never fall back to CPU");
#else
  static_cast<void>(model);
  static_cast<void>(image_path);
  throw std::runtime_error("B70 test requires a build with native OpenVINO support");
#endif
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
    verify_table_backend_routing();

    const char* models_dir = std::getenv("GRPARSE_TEST_MODELS_DIR");
    const char* data_dir = std::getenv("GRPARSE_TEST_DATA_DIR");
    const fs::path model =
        fs::path(models_dir == nullptr ? "models" : models_dir) / "slanet_plus.onnx";
    const fs::path image =
        fs::path(data_dir == nullptr ? "tests/data" : data_dir) / "span_table.png";
    const char* b70 = std::getenv("GRPARSE_TEST_B70");
    const bool strict_b70 = b70 != nullptr && std::string(b70) == "1";
    if (strict_b70) require(fs::exists(model), "strict B70 test requires the model");
    if (!fs::exists(model)) {
      std::println(stderr, "table-structure-engine-test: skipped, model not present: {:?}", model.string());
      return grparse_test::missing_model_exit_code();
    }
    require(fs::exists(image), "test image missing: " + image.string());

    if (strict_b70) {
      verify_b70(model, image);
      return EXIT_SUCCESS;
    }
    verify_rejects_empty_image(model);
    verify_span_table_matches_reference(model, image);
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "table-structure-engine-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
