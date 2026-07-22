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

// SLANet-plus table structure recognition (RapidTable export) on ONNX
// Runtime.  The session binds the process-wide execution provider selection,
// pooled exactly like OCR and layout sessions.
//
// Anti-seesaw contract: recognize is a batch=1 device call on a table crop
// of the raster the inference stage already holds; it neither retains the
// crop nor blocks on anything but its own inference.
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

// Warm exclusive-lease pool over TableStructureEngine, one session per slot.
class TableStructureEnginePool final : public TableStructurer {
 public:
  using Stats = ResourcePool<TableStructureEngine>::Stats;

  // Builds every session eagerly so a bad model or provider fails startup.
  TableStructureEnginePool(const std::filesystem::path& model_path, size_t worker_count);

  TableStructure recognize(const cv::Mat& crop) override;
  size_t size() const { return engines_.capacity(); }
  Stats stats() const { return engines_.stats(); }

 private:
  ResourcePool<TableStructureEngine> engines_;
};

}  // namespace grparse
