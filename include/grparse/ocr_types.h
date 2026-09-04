#pragma once

#include <cstdint>
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
  // The dominant font of the line, when the text layer declares one
  // (digital PDF text; raster OCR never knows). Size is in points,
  // independent of the page's raster scale; the name has any embedded
  // subset prefix stripped.
  std::optional<std::string> font_name = std::nullopt;
  std::optional<double> font_size_pt = std::nullopt;
  // True when the angle classifier turned the crop 180 degrees before the
  // recognizer read it.  RapidOCR's most-angle vote sets it for every line
  // of a page at once, so it is a page-level upside-down signal
  // (text_geometry.h, page_read_quality).
  bool flipped = false;
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

// One predicted figure class with its softmax probability.
struct FigureClass {
  std::string label;
  float confidence = 0.0F;
};

// One decoded barcode payload.  The format is ZXing's name for the symbology
// (for example "QRCode" or "Code128"); the text is the decoded content.
struct BarcodeResult {
  std::string format;
  std::string text;
};

// One detected layout region in page pixel coordinates (top-left origin,
// the same space text boxes use).  Boxes are corners, edges inclusive.
struct LayoutRegion {
  // The detector's own class name, not a normalized vocabulary: the
  // 17-label set the query detector emits (caption, list_item, page_header,
  // section_header, table, text, code, key_value_region, ...) or picodet's
  // five (text, title, list, table, picture).  Both are spelled out in
  // layout_labels(), src/layout_decode.cpp.
  std::string label;
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
  // Classifier output sorted by confidence, filled only for figure regions
  // when the figure classifier is active.
  std::vector<FigureClass> figure_classes = {};
  // Barcode payloads decoded from the region crop, filled only for figure
  // regions when barcode decoding triggers (by class or by flag).
  std::vector<BarcodeResult> barcodes = {};
};

// One consensus word linked to the same word in another backend's stream;
// mirrors parse_stream.proto's ReconciliationLink. Offsets are codepoint
// positions within each side's own page word stream.
struct ReconciliationLink {
  uint32_t consensus_index = 0;
  uint64_t consensus_utf_start = 0;
  std::optional<uint32_t> source_index = std::nullopt;
  uint64_t source_utf_start = 0;
  bool text_deviation = false;
  bool order_break = false;
};

// One non-winning backend's alignment against the page's consensus stream,
// built by the consensus page source and carried onto the wire as PageData
// transport metadata.
struct SourceReconciliation {
  std::string source;
  double weight = 0.0;
  uint32_t matched = 0;
  uint32_t missing = 0;
  uint32_t order_breaks = 0;
  uint32_t text_deviations = 0;
  std::vector<ReconciliationLink> links = {};
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
  // Which detector produced `regions`, for item source attribution; empty
  // when no layout model ran.
  std::string layout_model = {};
  // PNG-encoded downscaled preview of the page raster, captured in the
  // inference stage while the raster is alive.  Filled only when page-image
  // capture is enabled; empty otherwise.  Its pixel size may differ from
  // width/height (which can be PDF points); the aspect ratio matches.
  std::vector<unsigned char> preview_png = {};
  // The clockwise turn (0, 90, 180 or 270) the scheduler applied to the
  // raster before the read it kept.  Everything on the page (lines,
  // regions, width and height, the preview) is in the turned, upright
  // frame; the value tells a client that renders the source itself how to
  // bring its own image into that frame.  Wire slot:
  // PageItem.quality.rotation_degrees.
  int rotation_degrees = 0;
  // Multi-backend reconciliation, filled only by the consensus page
  // source. Wire slot: PageData.reconciliation.
  std::vector<SourceReconciliation> reconciliation = {};
  // Set by the consensus page source when the vote's winner reconciled
  // cleanly against every losing leg (nearly all words matched, nearly no
  // order breaks; the thresholds live in consensus_page_source.cpp). It
  // promises that the emission order of `lines` is already the page's
  // reading order, so the fold keeps it instead of re-deriving one from
  // geometry. Anything that re-sorts the lines (the digital+OCR merge)
  // builds a fresh page and leaves this false.
  bool source_order_trusted = false;
};

// Merge OCR lines into a digital page without duplicating geometry-overlapping text.
// Digital lines win on overlap; OCR fills the gaps. Result is reading-order sorted.
OcrPage merge_digital_and_ocr(OcrPage digital, OcrPage ocr);

}  // namespace grparse
