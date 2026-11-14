#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/chart_derender.h"

namespace grparse {

// Docling's code/formula crop recipe (docling CodeFormulaModel and
// CodeFormulaVlmModel): the page rendered at 120 DPI, the item's box grown by
// 0.18 of its own width and height on every side. The enrich service reads
// code and formula items from these crops, so the model sees the image the
// reference pipeline would hand it instead of the text layer alone.
inline constexpr double kCodeFormulaRenderDpi = 120.0;
inline constexpr double kCodeFormulaExpansion = 0.18;

// One item's crop in its page's own coordinates, top-left origin.
struct CodeFormulaBox {
  int page_no = 0;
  double left = 0.0;
  double top = 0.0;
  double right = 0.0;
  double bottom = 0.0;
};

// The box Docling would crop for an item. Docling's formula carries one
// provenance box; ours carries one per text run, so the item's box is the
// union of its boxes on its first page. Grown by `expansion` of the union's
// width and height on each side. nullopt when the item has no page box or
// its page has no size.
std::optional<CodeFormulaBox> code_formula_box(
    const ai::pipestream::document::v1::Document& document,
    const google::protobuf::RepeatedPtrField<ai::pipestream::document::v1::ProvenanceItem>& prov,
    double expansion = kCodeFormulaExpansion);

// The pixel rectangle of `box` in a raster of `raster` size rendered from a
// page measuring `page_width` x `page_height` in the document's units,
// clamped to the raster. Empty when nothing of the box is on the raster.
cv::Rect code_formula_pixels(const CodeFormulaBox& box, double page_width, double page_height,
                             cv::Size raster);

// Renders one page (1-indexed) or returns an empty Mat.
using CodeFormulaPageRenderer = std::function<cv::Mat(int page_no)>;

// One ItemImage per code item without its own image (when `code`) and per
// formula item (when `formula`), keyed by the self_ref the enrich service
// selects the item under ("#/texts/<index>" when the item has none). Each
// page renders at most once. Items whose crop is empty are left out, and
// the enrich service then falls back to their text.
std::vector<ChartCandidate> code_formula_crops(const ai::pipestream::document::v1::Document& document,
                                               bool code, bool formula, double expansion,
                                               const CodeFormulaPageRenderer& render);

// code_formula_crops over the source PDF, rendered through the PDF backend
// at kCodeFormulaRenderDpi. Empty when the bytes are not a PDF, nothing is
// asked for, or the source does not open; a crop is an aid, never a reason
// to fail the enrich leg. `stop` is polled before each page.
std::vector<ChartCandidate> render_code_formula_crops(
    const std::shared_ptr<const std::string>& bytes,
    const ai::pipestream::document::v1::Document& document, bool code, bool formula,
    double expansion, const std::function<bool()>& stop,
    std::chrono::system_clock::time_point deadline);

// The crops one enrich call carries: render_code_formula_crops for the jobs
// `options` turns on (the request's expansion factor, else Docling's), and
// none when enrich is not wired or neither job is on.
std::vector<ChartCandidate> enrich_item_crops(const ChartDerenderOptions& options,
                                              const std::shared_ptr<const std::string>& bytes,
                                              const ai::pipestream::document::v1::Document& document,
                                              const std::function<bool()>& stop,
                                              std::chrono::system_clock::time_point deadline);

}  // namespace grparse
