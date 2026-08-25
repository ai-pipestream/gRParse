#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "grparse/ocr_types.h"

namespace grparse {

// Which layout detector this process runs.  Both decoders stay compiled in;
// the selection is made once at startup and never varies per request.
enum class LayoutModel {
  // Query-based 17-label detector: the default.
  kHeron,
  // The legacy 5-label anchor-free detector, kept for comparison runs.
  kPicoDet,
};

// Reads GRPARSE_LAYOUT_MODEL: "heron" (default) or "picodet".  Throws
// std::invalid_argument on any other value rather than silently choosing.
LayoutModel configured_layout_model();

// Wire name of the selection, for logs and error messages.
std::string_view layout_model_name(LayoutModel model);

// File name the model is provisioned under inside the models directory.
std::string_view layout_model_file(LayoutModel model);

// Label set of a model, index == model class id.
const std::vector<std::string>& layout_labels(LayoutModel model);

// Score a detection must clear to survive.  Indexed by class id of the given
// model; out-of-range ids get the strictest gate rather than passing.
float layout_label_threshold(LayoutModel model, int class_id);

// The deterministic order downstream assembly depends on: confidence
// descending, then top, then left.
void sort_regions(std::vector<LayoutRegion>& regions);

// Decodes one batch element of a query-based detector head.  The head emits a
// fixed number of queries, each with a class id, a score, and a box already in
// the original page's pixel space (xyxy) - there is no anchor decode and no
// NMS to run.  Detections are dropped by the engine score gate, then by the
// per-label gate, boxes are clipped to the page, and the label remap the
// reference pipeline applies (title reads as a section header) lands here.
// Returns regions in sort_regions order.
std::vector<LayoutRegion> decode_query_detector(const int64_t* labels, const float* boxes,
                                                const float* scores, size_t queries, int width,
                                                int height);

// The engine-level gate applied before the per-label gates.
inline constexpr float kQueryDetectorScoreGate = 0.3F;

// Query count of the layout detector's head.
inline constexpr size_t kQueryDetectorQueries = 300;

}  // namespace grparse
