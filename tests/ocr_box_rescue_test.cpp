// Proves the low-score rescue of OCR boxes (src/ocr_engine.cpp): the
// detector filters at the lower threshold, a box at or above
// GRPARSE_OCR_BOX_SCORE always stays, and a box between the two stays only
// when its read is confident.

#include "grparse/ocr_engine.h"
#include "support/check.h"

namespace {

using grparse_test::require;

void verify_defaults() {
  const grparse::OcrDetectOptions options;
  require(grparse::detect_box_score(options) == 0.5F, "the detector filters at the rescue score");
  require(grparse::keep_ocr_line(0.61F, 0.10F, options), "a box over 0.6 stays at any confidence");
  require(grparse::keep_ocr_line(0.55F, 0.95F, options), "an underlined line that reads cleanly stays");
  require(!grparse::keep_ocr_line(0.55F, 0.70F, options), "a low-score box read with low confidence goes");
  require(!grparse::keep_ocr_line(0.45F, 0.99F, options), "a box under the rescue score goes");
}

void verify_rescue_off() {
  grparse::OcrDetectOptions options;
  options.rescue_box_score = 0.6F;
  require(grparse::detect_box_score(options) == 0.6F, "a rescue score at the box score turns the rescue off");
  require(!grparse::keep_ocr_line(0.55F, 0.99F, options), "with the rescue off a 0.55 box goes");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("ocr-box-rescue-test", "all checks passed", {
      verify_defaults,
      verify_rescue_off,
  });
}
