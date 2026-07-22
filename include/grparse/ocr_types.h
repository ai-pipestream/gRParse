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

// One detected layout region in page pixel coordinates (top-left origin,
// the same space text boxes use).  Boxes are corners, edges inclusive.
struct LayoutRegion {
  std::string label;  // text, title, list, table, figure
  float confidence = 0.0F;
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
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
