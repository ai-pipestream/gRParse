#pragma once

#include <vector>

#include <opencv2/core.hpp>

#include "grparse/ocr_engine.h"
#include "grparse/ocr_types.h"

namespace grparse {

// Orientation recovery for scanned pages.  A raster fed in sideways or
// upside down still yields text (RapidOCR transposes tall crops and its
// angle classifier flips crops read upside down) but its boxes are wrong
// for everything downstream: reading order, layout binding, furniture
// bands, the preview a client paints over.  The scheduler asks this module
// after a page's first read; when the read itself says the page was turned,
// the raster is re-read at the turns the evidence names and the best read,
// raster and all, replaces the first.
struct OrientationOptions {
  bool enabled = true;
  // A first read whose mean confidence falls below this, over at least
  // minimum_lines lines, is re-read at every turn even without a vote.
  float confidence_floor = 0.5F;
  int minimum_lines = 3;
};

struct OrientationOutcome {
  // The clockwise turn kept; 0 when the first read stood.
  int degrees = 0;
  // Extra recognition passes spent; 0 when nothing triggered.
  int passes = 0;
  // The turns tried, in order.
  std::vector<int> tried;
  float first_confidence = 0.0F;
  float kept_confidence = 0.0F;
};

// cv::rotate for a clockwise turn of 0, 90, 180 or 270 degrees; any other
// value throws std::invalid_argument.  0 returns the raster itself.
cv::Mat turn_raster(const cv::Mat& raster, int degrees);

// `page` is the first read of `raster`.  Decides from that read alone
// (text_geometry.h: page_rotation_vote, page_read_quality,
// rotation_candidates) whether the raster deserves re-reading turned 90,
// 180 or 270 degrees clockwise, reads it at each candidate turn once, and
// keeps the read that scores best (score_read: text over none, upright over
// turned, then mean confidence).  When a turn wins, `raster` becomes the
// turned raster and `page` its read with rotation_degrees set, so the
// caller's layout, crops and preview all happen in the upright frame.  Never
// call it for a page with a digital text layer: the source already applied
// the page's own rotation to that.
OrientationOutcome recover_orientation(PageRecognizer& recognizer, const OrientationOptions& options,
                                       cv::Mat* raster, OcrPage* page);

}  // namespace grparse
