#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"

namespace grparse {

struct AxisAlignedBox {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  // Widths and areas are computed in 64 bits: OCR polygons are model output and
  // a degenerate box must not silently wrap an int multiply.
  int64_t width() const { return static_cast<int64_t>(right) - left; }
  int64_t height() const { return static_cast<int64_t>(bottom) - top; }
  int64_t area() const { return std::max<int64_t>(0, width()) * std::max<int64_t>(0, height()); }
  cv::Point center() const {
    return {static_cast<int>((static_cast<int64_t>(left) + right) / 2),
            static_cast<int>((static_cast<int64_t>(top) + bottom) / 2)};
  }
  bool contains(cv::Point point) const {
    return point.x >= left && point.x <= right && point.y >= top && point.y <= bottom;
  }
};

AxisAlignedBox bounding_box(const OcrLine& line);
float intersection_over_union(const AxisAlignedBox& a, const AxisAlignedBox& b);
bool boxes_overlap_significantly(const AxisAlignedBox& a, const AxisAlignedBox& b,
                                 float iou_threshold = 0.25F);

// A text-direction vote over a page's line boxes. Text lines are wide and
// short; on a page rasterized a quarter turn from upright the detector
// still finds the lines, but as tall narrow boxes. The vote counts both
// shapes (a box is vertical when its height exceeds `aspect` times its
// width, horizontal when its width exceeds `aspect` times its height) and
// calls the page a quarter turn when at least `minimum_lines` lines were
// seen and vertical ones outnumber horizontal ones two to one. It cannot
// tell 90 from 270: the caller rotates the raster one way, re-recognizes,
// and keeps whichever orientation reads with the higher confidence.
struct RotationVote {
  int vertical_lines = 0;
  int horizontal_lines = 0;
  bool quarter_turn = false;
};

RotationVote page_rotation_vote(const OcrPage& page, double aspect = 1.5, int minimum_lines = 3);

// What one read of a page says about how well it read: the lines with
// text, the mean recognition confidence over the lines that carry one (0
// when none does), and how many lines the angle classifier turned 180
// degrees before reading.  With RapidOCR's most-angle vote the flipped
// count is all or nothing, so `upside_down` (more than half flipped) is
// the page-level signal that the raster was fed in upside down.
struct ReadQuality {
  int lines = 0;
  int scored_lines = 0;
  float mean_confidence = 0.0F;
  int flipped_lines = 0;
  bool upside_down = false;
};

ReadQuality page_read_quality(const OcrPage& page);

// The clockwise turns worth re-reading a page at, from its first read: 90
// and 270 when the line boxes vote a quarter turn (the vote cannot tell
// the two apart), 180 when the classifier flipped most lines, all three
// when neither says anything but the read is poor (mean confidence below
// `confidence_floor` over at least `minimum_lines` lines), nothing
// otherwise.  Each turn appears at most once, so the caller's cost is
// bounded by three extra reads per page.
std::vector<int> rotation_candidates(const RotationVote& vote, const ReadQuality& quality,
                                     float confidence_floor = 0.5F, int minimum_lines = 3);

// How reads of one page rank against each other.  A read with text beats
// one without; an upright read (wide lines at least as many as tall ones,
// no quarter-turn vote, not upside down) beats one that is not; among
// equals the higher mean confidence wins.
// Comparable with the relational operators in that order.
struct ReadScore {
  bool has_text = false;
  bool upright = false;
  float mean_confidence = 0.0F;

  auto operator<=>(const ReadScore&) const = default;
};

ReadScore score_read(const OcrPage& page);

}  // namespace grparse
