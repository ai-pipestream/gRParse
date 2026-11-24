#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "grparse/warning.h"

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
  // The structure model's mean token probability for `structured_cells`,
  // filled beside them; absent on geometry fallback. Feeds the document's
  // ConfidenceScores.table_score (confidence.h).
  std::optional<float> structure_score = std::nullopt;
  // Set when the structure model's decode ran out of steps before the end
  // of the table (TableStructure::truncated): how many of
  // `structured_cells`, from the front, the model placed. The cells from
  // that index on were built from line geometry (extend_truncated_table),
  // which adds none when no text lies under the model's last cell. Absent
  // when the decode reached its end or no structure model ran. Assembly
  // turns it into the table's typed warning, so a cut-off decode never
  // reads like a complete one.
  std::optional<size_t> structure_cut = std::nullopt;
  // Classifier output sorted by confidence, filled only for figure regions
  // when the figure classifier is active.
  std::vector<FigureClass> figure_classes = {};
  // Barcode payloads decoded from the region crop, filled only for figure
  // regions when barcode decoding triggers (by class or by flag).
  std::vector<BarcodeResult> barcodes = {};
  // Set on a picture region whose text was to be read from the sharper
  // render of its page (ocr_rect_render.h) and, that render having failed,
  // was read from the page raster at its lower resolution. Assembly turns
  // it into the picture item's typed warning, so the weaker read never
  // passes for the configured one. False when no sharper render was
  // configured for the source, when it succeeded, and on every region the
  // picture read does not cover.
  bool read_at_page_resolution = false;
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

// The consensus vote's record for one page (multi-backend PDF mode only).
// `reconciliation` carries the losing halves of the vote; this carries the
// winning half, which the document-level claim and the per-item source
// attribution are built from.
struct ConsensusVote {
  // One backend that read the page: the dialed target (the same name a
  // SourceReconciliation.source carries) and the engine name the backend
  // reported through Probe, empty when it did not name itself.
  struct Leg {
    std::string target;
    std::string engine;
  };
  // The leg whose word order won the page's bigram vote, and its score.
  Leg winner;
  double winner_score = 0.0;
  // Every leg that voted, winner included, in configured order.
  std::vector<Leg> legs;
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
  // Where the PDF places images on this page, in the same frame as `lines`
  // (digital pages only; empty otherwise). The OCR of a text-layer page reads
  // these (ocr_image_regions.h).
  std::vector<cv::Rect> image_placements = {};
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
  // The vote's winning half, filled by the consensus page source only when
  // at least two backends read the page: who won, with what score, and who
  // else voted. Absent in single-backend mode and on pages only one
  // backend could read, so neither place changes shape. Wire slot: the
  // document-level protomolt claim and per-item field_sources.
  std::optional<ConsensusVote> vote = std::nullopt;
  // Set by the consensus page source when the vote's winner reconciled
  // cleanly against every losing leg (nearly all words matched, nearly no
  // order breaks; the thresholds live in consensus_page_source.cpp). It
  // promises that the emission order of `lines` is already the page's
  // reading order, so the fold keeps it instead of re-deriving one from
  // geometry. Anything that re-sorts the lines (the digital+OCR merge)
  // builds a fresh page and leaves this false.
  bool source_order_trusted = false;
  // Set when replace_broken_text_layer caused this page's embedded layer to
  // be dropped and replaced by full-page OCR. Wire: PageQuality.text_layer_replaced.
  bool text_layer_replaced = false;
  // What the page's source said about its own read of the page: a PDF
  // backend that skipped or repaired content, a backend answer whose
  // trailer totals do not match what arrived. The scheduler copies them
  // from PageSource::page_warnings() onto the page it delivers, whichever
  // way the page was read, and the fold hands them to its caller's warnings.
  std::vector<Warning> source_warnings = {};
  // The sharper render this page's image reads were to come from
  // (ocr_rect_render.h) failed, so every one of them was read from the
  // page raster at its lower resolution instead.
  struct SharperRenderFailure {
    // Why, in the PDF backend's own words where it gave any.
    std::string cause;
    // How many image areas were read that way: the picture regions that
    // carry LayoutRegion::read_at_page_resolution, plus the PDF's own
    // image placements, which stand for no item.
    size_t reads = 0;
  };
  // Absent when the reads came from the sharper render, when the source
  // has none (a raster input, or an OCR DPI at or below the render DPI),
  // and when the page had no image area to read. Assembly turns it into
  // typed warnings: one for each picture item, and one for the page when
  // reads were made that no picture item stands for.
  std::optional<SharperRenderFailure> sharper_render_failure = std::nullopt;
};

// Merge OCR lines into a digital page without duplicating geometry-overlapping text.
// Digital lines win on overlap; OCR fills the gaps. Result is reading-order sorted.
OcrPage merge_digital_and_ocr(OcrPage digital, OcrPage ocr);

// Adds a recognizer's read of one region crop (a picture on a digital
// page: a chart whose labels live only in its bitmap) to `page`. The crop's
// lines shift by the crop origin into page space, take the OCR origin, and
// drop where they repeat a line the page already has (the text layer's own
// labels of a vector chart). Returns how many lines were added.
size_t add_region_read(OcrPage* page, OcrPage crop_read, int crop_left, int crop_top);

}  // namespace grparse
