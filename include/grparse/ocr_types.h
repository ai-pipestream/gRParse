#pragma once

#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace grparse {

enum class TextOrigin { kOcr, kDigitalPdf };

struct OcrLine {
  std::string text;
  std::vector<cv::Point> polygon;
  // Absent when the producer reports no per-line score.
  std::optional<float> confidence = std::nullopt;
  // When set, overrides OcrPage::source for assembly (digital+OCR merge).
  std::optional<TextOrigin> origin = std::nullopt;
};

// One table cell recognized by the structure model.  Grid coordinates are
// zero-based; spans cover [row, row + row_span) x [col, col + col_span).
// The box is axis-aligned in the same pixel space as the owning region.
struct StructuredCell {
  int row = 0;
  int col = 0;
  int row_span = 1;
  int col_span = 1;
  // True when the model placed the cell inside a <thead> section.
  bool header = false;
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

// One detected layout region in page pixel coordinates (top-left origin,
// the same space text boxes use).  Boxes are corners, edges inclusive.
struct LayoutRegion {
  std::string label;  // text, title, list, table, figure
  float confidence = 0.0F;
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  // PNG-encoded crop of the region, captured in the inference stage while the
  // raster is alive.  Filled only for figure regions when picture-image
  // capture is enabled; empty otherwise.
  std::vector<unsigned char> image_png = {};
  // Model-recognized cell grid, filled only for table regions when a table
  // structure engine is active; empty means geometry fallback.
  std::vector<StructuredCell> structured_cells = {};
};

struct OcrPage {
  enum class Source { kOcr, kDigitalPdf, kMerged };

  int width = 0;
  int height = 0;
  std::vector<OcrLine> lines;
  Source source = Source::kOcr;
  // When true, the scheduler may skip raster OCR (full digital coverage).
  bool skip_ocr = false;
  // Layout detections for this page; empty when no layout model is active.
  std::vector<LayoutRegion> regions = {};
};

// Merge OCR lines into a digital page without duplicating geometry-overlapping text.
// Digital lines win on overlap; OCR fills the gaps. Result is reading-order sorted.
OcrPage merge_digital_and_ocr(OcrPage digital, OcrPage ocr);

}  // namespace grparse
