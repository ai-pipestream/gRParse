// Proves the OCR input buckets and the shape ledger without any model
// (compat/grparse_ocr_shapes.h): every bucket covers its input and is itself
// a bucket, the lists hold a small fixed number of shapes, auto follows the
// execution provider, and the ledger counts distinct shapes per net.

#include <algorithm>
#include <cstdlib>
#include <format>
#include <set>
#include <stdexcept>
#include <stdlib.h>
#include <string>
#include <utility>

#include "grparse_session_ep.h"
#include "support/check.h"

namespace {

using grparse::OcrDetSize;
using grparse::OcrNet;
using grparse_test::require;
using grparse_test::require_equal;

// The detector's largest side at the default GRPARSE_OCR_MAX_SIDE: 2048 plus
// the 50 pixel border on both sides, rounded down to a multiple of 32.
constexpr int kLargestDetSide = 2144;

void set_env(const char* name, const char* value) {
  if (value == nullptr) {
    unsetenv(name);
  } else {
    setenv(name, value, 1);
  }
  grparse::reset_ocr_shape_buckets();
}

void select(grparse::OrtEp ep, const std::string& openvino_device = "GPU") {
  grparse::OrtEpSelection selection;
  selection.ep = ep;
  selection.openvino_device = openvino_device;
  grparse::set_ort_ep_selection(selection);
}

void verify_detector_buckets() {
  set_env("GRPARSE_OCR_SHAPE_BUCKETS", "on");
  std::set<std::pair<int, int>> sizes;
  for (int width = 32; width <= kLargestDetSide; width += 32) {
    for (int height = 32; height <= kLargestDetSide; height += 32) {
      const OcrDetSize bucket = grparse::ocr_det_bucket(width, height);
      require(bucket.width >= width && bucket.height >= height,
              std::format("{}x{} is not covered by its bucket {}x{}", width, height, bucket.width,
                          bucket.height));
      require(bucket.width % 32 == 0 && bucket.height % 32 == 0,
              "a bucket keeps the detector's multiple-of-32 sides");
      require(grparse::ocr_det_bucket(bucket.width, bucket.height) == bucket,
              "a bucket is its own bucket");
      sizes.insert({bucket.width, bucket.height});
    }
  }
  require_equal(sizes.size(), grparse::ocr_det_bucket_count(),
                "every listed detector size is used and nothing else is");
  require(sizes.size() <= 10,
          std::format("the detector runs at most ten sizes, the list has {}", sizes.size()));

  // The page sizes full-page reads produce are buckets themselves: a 200 DPI
  // letter page, upright and turned, pays no padding, and an A4 page pads
  // only its width.
  require(grparse::ocr_det_bucket(1664, 2144) == OcrDetSize{1664, 2144}, "upright letter page");
  require(grparse::ocr_det_bucket(2144, 1664) == OcrDetSize{2144, 1664}, "turned letter page");
  require(grparse::ocr_det_bucket(1536, 2144) == OcrDetSize{1664, 2144}, "upright A4 page");
  // Past the list (GRPARSE_OCR_MAX_SIDE above its default) each side rounds
  // up on its own and still covers.
  const OcrDetSize large = grparse::ocr_det_bucket(3000, 4100);
  require(large.width >= 3000 && large.height >= 4100 && large.width % 32 == 0 &&
              large.height % 32 == 0,
          "a size past the list is covered");
  require(grparse::ocr_det_bucket(large.width, large.height) == large,
          "a bucket past the list is its own bucket");
}

void verify_recognizer_buckets() {
  set_env("GRPARSE_OCR_SHAPE_BUCKETS", "on");
  std::set<int> widths;
  int previous = 0;
  for (int width = 1; width <= 3200; ++width) {
    const int bucket = grparse::ocr_rec_bucket(width);
    require(bucket >= width, "a bucket always covers the line");
    require(bucket >= previous, "wider lines never get a narrower bucket");
    require(grparse::ocr_rec_bucket(bucket) == bucket, "a bucket is its own bucket");
    // Padding stays a bounded share of the line: at most half as much again,
    // or 64 pixels for the shortest crops.
    require(bucket <= width + std::max(64, width / 2),
            std::format("width {} pads to {}", width, bucket));
    widths.insert(bucket);
    previous = bucket;
  }
  require_equal(widths.size(), grparse::ocr_rec_bucket_count(3200),
                "the recognizer runs exactly the listed widths");
  require(widths.size() <= 20,
          std::format("the recognizer runs at most twenty widths up to 3200, the list has {}",
                      widths.size()));
  require(grparse::ocr_rec_bucket(6000) >= 6000 &&
              grparse::ocr_rec_bucket(grparse::ocr_rec_bucket(6000)) == grparse::ocr_rec_bucket(6000),
          "a line past the list is covered by a bucket");
}

void verify_modes_pick_the_nets() {
  set_env("GRPARSE_OCR_SHAPE_BUCKETS", "off");
  require(!grparse::ocr_shape_buckets(), "off buckets nothing");
  require(grparse::ocr_det_bucket(333, 97) == OcrDetSize{333, 97} &&
              grparse::ocr_rec_bucket(333) == 333,
          "off leaves every size alone");
  set_env("GRPARSE_OCR_SHAPE_BUCKETS", "det");
  require(grparse::ocr_shape_buckets() && grparse::ocr_rec_bucket(333) == 333 &&
              grparse::ocr_det_bucket(320, 96).width >= 320,
          "det buckets the detector only");
  set_env("GRPARSE_OCR_SHAPE_BUCKETS", "rec");
  require(grparse::ocr_shape_buckets() && grparse::ocr_rec_bucket(333) > 333 &&
              grparse::ocr_det_bucket(320, 96) == OcrDetSize{320, 96},
          "rec buckets the recognizer only");
}

// auto: both nets on the OpenVINO provider with a GPU device, nothing
// anywhere else, and a new selection is picked up without a restart.
void verify_auto_follows_the_provider() {
  set_env("GRPARSE_OCR_SHAPE_BUCKETS", nullptr);
  select(grparse::OrtEp::kCpu);
  require(!grparse::ocr_shape_buckets(), "auto on CPU buckets nothing");
  select(grparse::OrtEp::kCuda);
  require(!grparse::ocr_shape_buckets(), "auto on CUDA buckets nothing");
  select(grparse::OrtEp::kOpenVino, "CPU");
  require(!grparse::ocr_shape_buckets(), "auto on the OpenVINO CPU device buckets nothing");
  for (const char* device : {"GPU", "GPU.1"}) {
    select(grparse::OrtEp::kOpenVino, device);
    require(grparse::ocr_shape_buckets() && grparse::ocr_rec_bucket(333) > 333 &&
                grparse::ocr_det_bucket(320, 96) != OcrDetSize{320, 96},
            std::format("auto on the OpenVINO {} device buckets both nets", device));
  }
  select(grparse::OrtEp::kCpu);
  require(!grparse::ocr_shape_buckets(), "a later CPU selection turns auto off again");
}

void verify_step_and_pad_settings() {
  set_env("GRPARSE_OCR_SHAPE_BUCKETS", "on");
  set_env("GRPARSE_OCR_DET_STEP", "64");
  require(grparse::ocr_det_bucket(65, 640) == OcrDetSize{128, 640},
          "GRPARSE_OCR_DET_STEP rounds each side up to a multiple of it");
  set_env("GRPARSE_OCR_DET_STEP", nullptr);
  set_env("GRPARSE_OCR_REC_STEP", "96");
  require(grparse::ocr_rec_bucket(100) == 192, "GRPARSE_OCR_REC_STEP rounds the width up");
  set_env("GRPARSE_OCR_REC_STEP", nullptr);
  using grparse::OcrPad;
  for (const char* mode : {static_cast<const char*>(nullptr), "auto", "tile"}) {
    set_env("GRPARSE_OCR_BUCKET_PAD", mode);
    require(grparse::ocr_det_pad() == OcrPad::kTile && grparse::ocr_rec_pad() == OcrPad::kConstant,
            "by default the detector input is tiled and a line is padded with zeros");
  }
  set_env("GRPARSE_OCR_BUCKET_PAD", "replicate");
  require(grparse::ocr_det_pad() == OcrPad::kReplicate &&
              grparse::ocr_rec_pad() == OcrPad::kReplicate,
          "replicate repeats the edge pixels for both nets");
  set_env("GRPARSE_OCR_BUCKET_PAD", "zero");
  require(grparse::ocr_det_pad() == OcrPad::kConstant && grparse::ocr_rec_pad() == OcrPad::kConstant,
          "zero pads both nets with a constant");
  set_env("GRPARSE_OCR_BUCKET_PAD", nullptr);

  for (const auto& [name, value] : {std::pair{"GRPARSE_OCR_SHAPE_BUCKETS", "sometimes"},
                                    std::pair{"GRPARSE_OCR_DET_STEP", "50"},
                                    std::pair{"GRPARSE_OCR_REC_STEP", "-32"},
                                    std::pair{"GRPARSE_OCR_BUCKET_PAD", "black"}}) {
    set_env(name, value);
    bool threw = false;
    try {
      (void)grparse::ocr_shape_buckets();
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    require(threw, std::format("{}={} is rejected", name, value));
    set_env(name, name == std::string("GRPARSE_OCR_SHAPE_BUCKETS") ? "on" : nullptr);
  }
}

void verify_ledger_counts_distinct_shapes() {
  grparse::reset_ocr_input_shapes();
  require(grparse::ocr_input_shapes().empty(), "a reset ledger is empty");
  grparse::note_ocr_input_shape(OcrNet::kRec, 1, 48, 320);
  grparse::note_ocr_input_shape(OcrNet::kRec, 1, 48, 160);
  grparse::note_ocr_input_shape(OcrNet::kRec, 1, 48, 320);
  grparse::note_ocr_input_shape(OcrNet::kRec, 16, 48, 320);
  grparse::note_ocr_input_shape(OcrNet::kDet, 1, 2144, 1664);
  grparse::note_ocr_input_shape(OcrNet::kCls, 1, 48, 192);
  require_equal(grparse::ocr_input_shape_count(OcrNet::kDet), size_t{1}, "one detector shape");
  require_equal(grparse::ocr_input_shape_count(OcrNet::kCls), size_t{1}, "one angle shape");
  require_equal(grparse::ocr_input_shape_count(OcrNet::kRec), size_t{3},
                "a batch size is part of the shape, a repeat is not a new one");

  const auto shapes = grparse::ocr_input_shapes();
  require_equal(shapes.size(), size_t{5}, "five distinct shapes in all");
  require(shapes.front().net == OcrNet::kDet && shapes.front().width == 1664 &&
              shapes.front().height == 2144 && shapes.front().runs == 1,
          "the detector shape comes first with its size and run count");
  require(shapes[2].net == OcrNet::kRec && shapes[2].width == 160 && shapes[3].width == 320 &&
              shapes[3].batch == 1 && shapes[3].runs == 2 && shapes[4].batch == 16,
          "recognizer shapes are ordered by width, then batch, with their run counts");
  require(grparse::ocr_net_name(OcrNet::kDet) == "det" &&
              grparse::ocr_net_name(OcrNet::kCls) == "cls" &&
              grparse::ocr_net_name(OcrNet::kRec) == "rec",
          "the nets carry their metric labels");
  grparse::reset_ocr_input_shapes();
  require_equal(grparse::ocr_input_shape_count(OcrNet::kRec), size_t{0}, "reset empties the ledger");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("ocr-shape-buckets-test", "all checks passed", {
      verify_detector_buckets,
      verify_recognizer_buckets,
      verify_modes_pick_the_nets,
      verify_auto_follows_the_provider,
      verify_step_and_pad_settings,
      verify_ledger_counts_distinct_shapes,
  });
}
