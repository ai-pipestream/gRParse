#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"
#include "grparse/resource_pool.h"
#include "grparse/text_geometry.h"

namespace grparse {

// Model-recognized table structure in crop pixel coordinates.
struct TableStructure {
  int rows = 0;
  int cols = 0;
  // Mean probability over the emitted structure tokens.
  float score = 0.0F;
  std::vector<StructuredCell> cells;
};

// Places the decoded HTML structure tokens on a grid.  `tokens` is the raw
// tag stream (`<tr>`, `<td`, span attributes, `>`, `</td>`, `<td></td>`,
// thead/tbody wrappers are ignored); `cell_boxes` holds one box per cell
// token in emission order.  Cells go to the first free column of their row
// with rowspan occupancy carried down, the SLANet/HTML placement rule.
// Exposed for tests: the parse must be right independently of any model.
TableStructure structure_from_tokens(const std::vector<std::string>& tokens,
                                     const std::vector<AxisAlignedBox>& cell_boxes);

// The scheduler talks to this interface so tests can structure tables
// without a model.
class TableStructurer {
 public:
  virtual ~TableStructurer() = default;
  virtual TableStructure recognize(const cv::Mat& crop) = 0;
};

// SLANet-plus table recognition binds the process-wide provider selection.
// CPU/CUDA and OpenVINO NPU use ONNX Runtime with the original model.
// Only OpenVINO GPU/GPU.<n> use native OpenVINO with the sibling
// slanet_plus_openvino.onnx and raw vocabulary slanet_plus_openvino.tokens.
// Native GPU compilation is fresh per call because the shipped runtime retains
// Loop decoder state across requests (~0.4s warm-cache compile cost on B70).
// Construction compiles eagerly; accelerator failures never select a CPU backend.
// NPU requests retain the strict OpenVINO EP path; hardware/model support
// must be validated on an NPU and is not established by the B70 golden test.
//
// Anti-seesaw contract: recognize is a batch=1 device call on a table crop
// of the raster the inference stage already holds; it neither retains the
// crop nor blocks on anything but compilation/inference and the GPU JIT gate.
class TableStructureEngine final {
 public:
  // Throws when the model is missing or the configured execution provider
  // cannot initialize (startup fail-loud, same policy as OCR).
  explicit TableStructureEngine(const std::filesystem::path& model_path);
  ~TableStructureEngine();
  TableStructureEngine(const TableStructureEngine&) = delete;
  TableStructureEngine& operator=(const TableStructureEngine&) = delete;

  // BGR table crop in, cell grid in that crop's pixel space out.
  TableStructure recognize(const cv::Mat& crop);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Exclusive-lease pool over TableStructureEngine, one engine per slot.
class TableStructureEnginePool final : public TableStructurer {
 public:
  using Stats = ResourcePool<TableStructureEngine>::Stats;

  // Initializes every engine eagerly so a bad model or provider fails startup.
  TableStructureEnginePool(const std::filesystem::path& model_path, size_t worker_count);

  TableStructure recognize(const cv::Mat& crop) override;
  size_t size() const { return engines_.capacity(); }
  Stats stats() const { return engines_.stats(); }

 private:
  ResourcePool<TableStructureEngine> engines_;
};

}  // namespace grparse
