#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "grparse/layout_decode.h"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

constexpr int kPageWidth = 1000;
constexpr int kPageHeight = 1400;

// One synthetic head output: every query starts scored below the engine gate
// with a degenerate box, so a test only has to fill the queries it cares
// about.  Sized like the real head so the decode is exercised at its own
// query count.
struct Head {
  Head() {
    labels.assign(grparse::kQueryDetectorQueries, 9);  // text
    scores.assign(grparse::kQueryDetectorQueries, 0.0F);
    boxes.assign(grparse::kQueryDetectorQueries * 4, 0.0F);
  }

  void set(size_t query, int64_t label, float score, float left, float top, float right,
           float bottom) {
    labels[query] = label;
    scores[query] = score;
    boxes[query * 4 + 0] = left;
    boxes[query * 4 + 1] = top;
    boxes[query * 4 + 2] = right;
    boxes[query * 4 + 3] = bottom;
  }

  std::vector<grparse::LayoutRegion> decode() const {
    return grparse::decode_query_detector(labels.data(), boxes.data(), scores.data(),
                                          grparse::kQueryDetectorQueries, kPageWidth, kPageHeight);
  }

  std::vector<int64_t> labels;
  std::vector<float> scores;
  std::vector<float> boxes;
};

bool has_label(const std::vector<grparse::LayoutRegion>& regions, const std::string& label) {
  return std::ranges::any_of(regions,
                             [&](const grparse::LayoutRegion& r) { return r.label == label; });
}

void verify_label_set() {
  const auto& heron = grparse::layout_labels(grparse::LayoutModel::kHeron);
  require(heron.size() == 17, "the query detector predicts 17 labels");
  require(heron[0] == "caption" && heron[6] == "picture" && heron[9] == "text" &&
              heron[16] == "key_value_region",
          "label ids must match the model's own id2label order");
  const auto& legacy = grparse::layout_labels(grparse::LayoutModel::kPicoDet);
  require(legacy.size() == 5 && legacy[4] == "picture",
          "the legacy detector keeps five labels and speaks the shared picture name");
}

void verify_model_selection() {
  require(grparse::layout_model_file(grparse::LayoutModel::kHeron) == "layout_heron.onnx" &&
              grparse::layout_model_file(grparse::LayoutModel::kPicoDet) ==
                  "layout_publaynet.onnx",
          "each model resolves to its own provisioned file name");

  unsetenv("GRPARSE_LAYOUT_MODEL");
  require(grparse::configured_layout_model() == grparse::LayoutModel::kHeron,
          "the query detector is the default");
  setenv("GRPARSE_LAYOUT_MODEL", "picodet", 1);
  require(grparse::configured_layout_model() == grparse::LayoutModel::kPicoDet,
          "picodet selects the legacy detector");
  setenv("GRPARSE_LAYOUT_MODEL", "", 1);
  require(grparse::configured_layout_model() == grparse::LayoutModel::kHeron,
          "an empty selection is the default, not an error");
  setenv("GRPARSE_LAYOUT_MODEL", "publaynet", 1);
  bool threw = false;
  try {
    (void)grparse::configured_layout_model();
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "an unknown selection must fail loudly rather than pick one");
  unsetenv("GRPARSE_LAYOUT_MODEL");
}

// The engine gate runs first; nothing under it reaches the per-label gates.
void verify_engine_score_gate() {
  Head head;
  head.set(0, 9, 0.29F, 10, 10, 200, 60);   // text, under the engine gate
  head.set(1, 9, 0.95F, 10, 100, 200, 160);  // text, well over it
  const auto regions = head.decode();
  require(regions.size() == 1, "only detections over the engine gate survive");
  require(regions.front().confidence > 0.9F, "the surviving detection is the confident one");
}

// Each label carries its own gate: the structural labels sit at 0.45, the
// rest at 0.5, and the comparison is inclusive at the gate.
void verify_per_label_gates() {
  Head head;
  head.set(0, 9, 0.49F, 10, 10, 200, 60);      // text under its 0.5 gate
  head.set(1, 7, 0.49F, 10, 100, 200, 160);    // section_header over its 0.45 gate
  head.set(2, 7, 0.45F, 10, 200, 200, 260);    // section_header exactly at its gate
  head.set(3, 7, 0.44F, 10, 300, 200, 360);    // section_header just under it
  head.set(4, 8, 0.5F, 10, 400, 200, 460);     // table exactly at its 0.5 gate
  const auto regions = head.decode();
  require(regions.size() == 3, "three detections clear their own gates");
  require(!has_label(regions, "text"), "text under 0.5 is dropped");
  for (const auto& region : regions) {
    require(region.confidence >= 0.45F, "nothing under the lowest gate survives");
  }
}

// A title detection is gated as a title and emitted as a section header.
void verify_title_remap() {
  Head head;
  head.set(0, 10, 0.46F, 10, 10, 200, 60);   // title over the 0.45 title gate
  head.set(1, 10, 0.44F, 10, 100, 200, 160);  // title under it
  const auto regions = head.decode();
  require(regions.size() == 1, "the title gate is the title's own, not the remap target's");
  require(regions.front().label == "section_header",
          "a title is emitted as a section header (got " + regions.front().label + ")");
}

void verify_boxes_clip_to_the_page() {
  Head head;
  head.set(0, 6, 0.9F, -40.0F, -12.0F, kPageWidth + 80.0F, kPageHeight + 5.0F);
  head.set(1, 6, 0.8F, 500.0F, 500.0F, 500.0F, 600.0F);  // zero width
  head.set(2, 6, 0.7F, 500.0F, 600.0F, 400.0F, 700.0F);  // inverted
  const auto regions = head.decode();
  require(regions.size() == 1, "degenerate and inverted boxes are dropped");
  const auto& region = regions.front();
  require(region.left == 0 && region.top == 0 && region.right == kPageWidth &&
              region.bottom == kPageHeight,
          "boxes clip to the page they were predicted for");
  require(region.label == "picture", "class 6 is the picture label");
}

// Boxes arrive in page pixels already, so the decode rounds rather than
// rescales; downstream assembly depends on that.
void verify_boxes_are_page_pixels() {
  Head head;
  head.set(0, 9, 0.9F, 120.4F, 72.6F, 850.5F, 148.49F);
  const auto region = head.decode().front();
  require(region.left == 120 && region.top == 73 && region.right == 851 && region.bottom == 148,
          "coordinates round to the nearest pixel of the original page");
}

// Confidence descending, then top, then left: assembly and the goldens both
// depend on this being total and stable.
void verify_deterministic_order() {
  Head head;
  head.set(0, 9, 0.80F, 10, 500, 200, 560);
  head.set(1, 9, 0.90F, 10, 300, 200, 360);
  head.set(2, 9, 0.80F, 10, 100, 200, 160);
  head.set(3, 9, 0.80F, 400, 100, 600, 160);  // same score and top, further right
  const auto regions = head.decode();
  require(regions.size() == 4, "every detection clears the text gate");
  require(regions[0].confidence > 0.85F, "the most confident detection leads");
  require(regions[1].top == 100 && regions[1].left == 10, "ties break on top, then left");
  require(regions[2].top == 100 && regions[2].left == 400, "the further-right tie follows");
  require(regions[3].top == 500, "the lowest box of the tie group is last");
}

void verify_unknown_class_ids_are_ignored() {
  Head head;
  head.set(0, 17, 0.99F, 10, 10, 200, 60);   // one past the last label
  head.set(1, -1, 0.99F, 10, 100, 200, 160);  // negative
  head.set(2, 9, 0.99F, 10, 200, 200, 260);
  const auto regions = head.decode();
  require(regions.size() == 1, "class ids outside the label set are dropped, not clamped");
  require(regions.front().label == "text", "the valid detection is unaffected");
}

// Every label the model can predict must survive its own gate and reach the
// shared vocabulary, so a new label never silently becomes plain text.
void verify_every_label_decodes() {
  Head head;
  for (int64_t label = 0; label < 17; ++label) {
    const auto query = static_cast<size_t>(label);
    const float top = 10.0F * static_cast<float>(label);
    head.set(query, label, 0.99F, 10.0F, top, 200.0F, top + 8.0F);
  }
  const auto regions = head.decode();
  require(regions.size() == 17, "all 17 predictions clear their gates");
  const auto& names = grparse::layout_labels(grparse::LayoutModel::kHeron);
  for (size_t index = 0; index < names.size(); ++index) {
    // Title never reaches assembly under its own name; it arrives remapped.
    const std::string& expected = names[index] == "title" ? "section_header" : names[index];
    require(has_label(regions, expected), "no region carried the " + expected + " label");
  }
}

void verify_null_inputs_are_rejected() {
  Head head;
  bool threw = false;
  try {
    (void)grparse::decode_query_detector(nullptr, head.boxes.data(), head.scores.data(), 1,
                                         kPageWidth, kPageHeight);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "a missing output tensor must be rejected");
}

}  // namespace

int main() {
  try {
    verify_label_set();
    verify_model_selection();
    verify_engine_score_gate();
    verify_per_label_gates();
    verify_title_remap();
    verify_boxes_clip_to_the_page();
    verify_boxes_are_page_pixels();
    verify_deterministic_order();
    verify_unknown_class_ids_are_ignored();
    verify_every_label_decodes();
    verify_null_inputs_are_rejected();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "layout-decode-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
