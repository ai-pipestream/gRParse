#include "grparse/layout_decode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace grparse {
namespace {

// Per-label score gates from the reference pipeline's layout postprocessor,
// indexed by class id.  Structural labels that the detector is more cautious
// about sit at the lower gate.
constexpr std::array<float, 17> kHeronThresholds = {
    0.5F,   // caption
    0.5F,   // footnote
    0.5F,   // formula
    0.5F,   // list_item
    0.5F,   // page_footer
    0.5F,   // page_header
    0.5F,   // picture
    0.45F,  // section_header
    0.5F,   // table
    0.5F,   // text
    0.45F,  // title
    0.45F,  // document_index
    0.45F,  // code
    0.45F,  // checkbox_selected
    0.45F,  // checkbox_unselected
    0.45F,  // form
    0.45F,  // key_value_region
};

// The one label remap the reference pipeline applies after thresholding: a
// title detection is emitted as a section header, so downstream hierarchy work
// sees a single heading vocabulary.
constexpr int kTitleClassId = 10;
constexpr int kSectionHeaderClassId = 7;

}  // namespace

LayoutModel configured_layout_model() {
  const char* configured = std::getenv("GRPARSE_LAYOUT_MODEL");
  const std::string selection =
      configured == nullptr || *configured == '\0' ? "heron" : configured;
  if (selection == "heron") return LayoutModel::kHeron;
  if (selection == "picodet") return LayoutModel::kPicoDet;
  throw std::invalid_argument("GRPARSE_LAYOUT_MODEL must be heron or picodet");
}

std::string_view layout_model_name(LayoutModel model) {
  return model == LayoutModel::kHeron ? "heron" : "picodet";
}

std::string_view layout_model_file(LayoutModel model) {
  return model == LayoutModel::kHeron ? "layout_heron.onnx" : "layout_publaynet.onnx";
}

const std::vector<std::string>& layout_labels(LayoutModel model) {
  static const std::vector<std::string> kHeronLabels = {
      "caption",           "footnote",           "formula", "list_item",
      "page_footer",       "page_header",        "picture", "section_header",
      "table",             "text",               "title",   "document_index",
      "code",              "checkbox_selected",  "checkbox_unselected",
      "form",              "key_value_region"};
  static const std::vector<std::string> kPicoDetLabels = {"text", "title", "list", "table",
                                                          "picture"};
  return model == LayoutModel::kHeron ? kHeronLabels : kPicoDetLabels;
}

float layout_label_threshold(LayoutModel model, int class_id) {
  if (class_id < 0) return 1.0F;
  if (model == LayoutModel::kPicoDet) {
    // The anchor-free decode thresholds every class identically and does so
    // per cell, before its own suppression pass; nothing else gates it.
    return class_id < static_cast<int>(layout_labels(model).size()) ? 0.5F : 1.0F;
  }
  return class_id < static_cast<int>(kHeronThresholds.size())
             ? kHeronThresholds[static_cast<size_t>(class_id)]
             : 1.0F;
}

void sort_regions(std::vector<LayoutRegion>& regions) {
  std::ranges::sort(regions, [](const LayoutRegion& a, const LayoutRegion& b) {
    if (a.confidence != b.confidence) return a.confidence > b.confidence;
    if (a.top != b.top) return a.top < b.top;
    return a.left < b.left;
  });
}

std::vector<LayoutRegion> decode_query_detector(const int64_t* labels, const float* boxes,
                                                const float* scores, size_t queries, int width,
                                                int height) {
  if (labels == nullptr || boxes == nullptr || scores == nullptr) {
    throw std::invalid_argument("Query detector decode needs labels, boxes, and scores");
  }
  const auto& names = layout_labels(LayoutModel::kHeron);
  std::vector<LayoutRegion> regions;
  for (size_t query = 0; query < queries; ++query) {
    const float score = scores[query];
    if (score < kQueryDetectorScoreGate) continue;
    const int64_t raw = labels[query];
    if (raw < 0 || raw >= static_cast<int64_t>(names.size())) continue;
    int class_id = static_cast<int>(raw);
    if (score < layout_label_threshold(LayoutModel::kHeron, class_id)) continue;
    // Threshold on the detected label, then remap: the gates above are keyed
    // to what the model actually predicted.
    if (class_id == kTitleClassId) class_id = kSectionHeaderClassId;

    const float* box = boxes + query * 4;
    LayoutRegion region;
    region.label = names[static_cast<size_t>(class_id)];
    region.confidence = score;
    region.left = std::clamp(static_cast<int>(std::lround(box[0])), 0, width);
    region.top = std::clamp(static_cast<int>(std::lround(box[1])), 0, height);
    region.right = std::clamp(static_cast<int>(std::lround(box[2])), 0, width);
    region.bottom = std::clamp(static_cast<int>(std::lround(box[3])), 0, height);
    if (region.right <= region.left || region.bottom <= region.top) continue;
    regions.push_back(std::move(region));
  }
  sort_regions(regions);
  return regions;
}

}  // namespace grparse
