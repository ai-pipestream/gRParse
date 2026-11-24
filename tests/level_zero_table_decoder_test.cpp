// The OpenVINO image's table structure path (encoder on native OpenVINO, the
// decoder on Level Zero) against the CPU path (the released slanet_plus.onnx
// on ONNX Runtime's CPU provider) on real table crops: the same grid, the
// same cells and the same boxes.  Crops decoded concurrently, which share
// decoder launches, must come back as they do alone.  Needs the models and
// an Intel GPU (skip 77 without either).
//
// Real crops, not random encoder features: on random features the decoder's
// recurrence is chaotic, and two CPU float runtimes of the same step graph
// already disagree by tens of logits within 200 steps, while on table
// features they agree within 1e-4.
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "grparse/table_structure_engine.h"
#include "grparse_session_ep.h"
#include "support/check.h"

namespace {

namespace fs = std::filesystem;
using grparse::TableStructure;
using grparse::TableStructureEngine;
using grparse_test::require;

void select(grparse::OrtEp ep) {
  grparse::OrtEpSelection selection;
  selection.ep = ep;
  selection.openvino_device = "GPU";
  grparse::set_ort_ep_selection(selection);
}

// The whole table and three regions of it, so the decodes differ in length.
std::vector<cv::Mat> crops_of(const cv::Mat& table) {
  const int w = table.cols, h = table.rows;
  return {table, table(cv::Rect(0, 0, w, h * 3 / 5)).clone(), table(cv::Rect(0, 0, w * 7 / 10, h)).clone(),
          table(cv::Rect(w / 5, h / 5, w * 3 / 5, h * 3 / 5)).clone()};
}

void require_same(const TableStructure& want, const TableStructure& got, const std::string& what) {
  require(want.rows == got.rows && want.cols == got.cols && want.truncated == got.truncated,
          std::format("{}: grid {}x{} on CPU, {}x{} on the GPU", what, want.rows, want.cols, got.rows, got.cols));
  require(want.cells.size() == got.cells.size(), what + ": cell count differs");
  for (size_t index = 0; index < want.cells.size(); ++index) {
    const auto& a = want.cells[index];
    const auto& b = got.cells[index];
    require(a.row == b.row && a.col == b.col && a.row_span == b.row_span && a.col_span == b.col_span &&
                a.header == b.header && a.left == b.left && a.top == b.top && a.right == b.right &&
                a.bottom == b.bottom,
            std::format("{}: cell {} differs", what, index));
  }
  require(std::abs(want.score - got.score) < 1e-4F, what + ": score differs");
}

}  // namespace

int main() {
  const char* models = std::getenv("GRPARSE_TEST_MODELS_DIR");
  const char* data = std::getenv("GRPARSE_TEST_DATA_DIR");
  const fs::path model = fs::path(models ? models : "models") / "slanet_plus.onnx";
  const fs::path image = fs::path(data ? data : "tests/data") / "span_table.png";
  if (!fs::is_regular_file(model) || !fs::is_regular_file(model.parent_path() / "slanet_plus_step.onnx")) {
    std::println("level-zero-table-decoder-test: skipped, no {} and its split files", model.string());
    return grparse_test::missing_model_exit_code();
  }
  const cv::Mat table = cv::imread(image.string(), cv::IMREAD_COLOR);
  if (table.empty()) {
    std::println(stderr, "level-zero-table-decoder-test: cannot read {}", image.string());
    return EXIT_FAILURE;
  }
  const std::vector<cv::Mat> crops = crops_of(table);
  select(grparse::OrtEp::kCpu);
  TableStructureEngine cpu(model);
  std::vector<TableStructure> want;
  for (const cv::Mat& crop : crops) want.push_back(cpu.recognize(crop));
  select(grparse::OrtEp::kOpenVino);
  std::vector<std::unique_ptr<TableStructureEngine>> gpu;
  try {
    for (size_t index = 0; index < crops.size(); ++index) {
      gpu.push_back(std::make_unique<TableStructureEngine>(model));
    }
  } catch (const std::exception& error) {
    std::println("level-zero-table-decoder-test: skipped, no Intel GPU path ({})", error.what());
    return 77;
  }
  return grparse_test::run_test_main(
      "level-zero-table-decoder-test", "GPU path matches the CPU path",
      {[&] {
         for (size_t index = 0; index < crops.size(); ++index) {
           require_same(want[index], gpu[0]->recognize(crops[index]), std::format("crop {} alone", index));
         }
       },
       [&] {
         std::vector<TableStructure> together(crops.size());
         std::vector<std::thread> threads;
         for (size_t index = 0; index < crops.size(); ++index) {
           threads.emplace_back([&, index] { together[index] = gpu[index]->recognize(crops[index]); });
         }
         for (std::thread& thread : threads) thread.join();
         for (size_t index = 0; index < crops.size(); ++index) {
           require_same(want[index], together[index], std::format("crop {} beside the others", index));
         }
       }});
}
