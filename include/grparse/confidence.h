#pragma once

#include <optional>
#include <vector>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "grparse/ocr_types.h"

namespace grparse {

// One page's read quality on the four upstream axes. An absent axis was not
// measured on the page (no OCR line, no table, no layout model, no vote);
// it never counts as zero.
struct PageConfidence {
  std::optional<double> parse_score;
  std::optional<double> layout_score;
  std::optional<double> table_score;
  std::optional<double> ocr_score;

  bool any() const;
  // The mean of the present axes; absent when none is present.
  std::optional<double> mean() const;
  // The 5th percentile of the present axes, linearly interpolated the way
  // the upstream grader takes it; absent when none is present.
  std::optional<double> low() const;
};

// Reads a page's axes off what the scheduler measured:
//   ocr     the mean confidence of the lines recognized from the raster
//   layout  the mean confidence of the layout regions
//   table   the mean structure score of the table regions that carry one
//   parse   the multi-backend vote's winning score, when the page voted
PageConfidence page_confidence(const OcrPage& page);

// The upstream grade buckets: below 0.5 POOR, below 0.8 FAIR, below 0.9
// GOOD, otherwise EXCELLENT.
ai::pipestream::parse::v1::QualityGrade quality_grade(double score);

// Folds the pages into the document-level report the way the upstream
// pipeline does: parse_score is the 10th percentile over pages (the worst
// pages decide the text layer's grade), the other axes are means over the
// pages that measured them, mean_score is the mean of the page means and
// low_score the mean of the page lows. Returns nothing when no page measured
// anything, so a caller never ships an empty report.
std::optional<ai::pipestream::parse::v1::ConfidenceScores> document_confidence(
    const std::vector<PageConfidence>& pages);

}  // namespace grparse
