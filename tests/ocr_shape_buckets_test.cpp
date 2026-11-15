// Proves the OCR input buckets (patches/rapidocr-shape-buckets.patch): the
// detector side rounds up to a multiple of 256, the recognizer width to its
// fixed list and then to multiples of 640, and every bucket covers its input
// (GRPARSE_OCR_SHAPE_BUCKETS=on for this process).

#include <cstdlib>
#include <set>
#include <stdlib.h>
#include <string>

#include "grparse_session_ep.h"
#include "support/check.h"

namespace {

using grparse_test::require;

void verify_detector_buckets() {
  require(grparse::ocr_det_bucket(1) == 256 && grparse::ocr_det_bucket(256) == 256, "256 holds up to 256");
  require(grparse::ocr_det_bucket(257) == 512 && grparse::ocr_det_bucket(960) == 1024, "rounds up by 256");
  require(grparse::ocr_det_bucket(2048) == 2048, "the default max side is its own bucket");
  require(grparse::ocr_det_bucket(0) == 256, "an empty side still gets a bucket");
  std::set<int> shapes;
  for (int side = 32; side <= 2048; side += 32) shapes.insert(grparse::ocr_det_bucket(side));
  require(shapes.size() == 8, "the detector sees 8 sizes per side up to 2048: " + std::to_string(shapes.size()));
}

void verify_recognizer_buckets() {
  require(grparse::ocr_rec_bucket(1) == 160 && grparse::ocr_rec_bucket(161) == 320, "short lines");
  require(grparse::ocr_rec_bucket(3200) == 3200 && grparse::ocr_rec_bucket(3201) == 3840, "past the list, by 640");
  for (int width = 1; width <= 6000; width += 7) {
    require(grparse::ocr_rec_bucket(width) >= width, "a bucket always covers the line");
  }
}

}  // namespace

int main() {
  // The mode is read once per process; "on" buckets both nets.
  setenv("GRPARSE_OCR_SHAPE_BUCKETS", "on", 1);
  return grparse_test::run_test_main("ocr-shape-buckets-test", "all checks passed", {
      verify_detector_buckets,
      verify_recognizer_buckets,
  });
}
