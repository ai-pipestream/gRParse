// Proves the document-level ConfidenceScores fold: which lines count as OCR,
// that an unmeasured axis stays absent rather than reading as zero, the
// upstream aggregation rules (parse the worst 10% of pages, the other axes
// page means, page mean/low folded by mean), the grade buckets, and that a
// parse with no measured page ships no report at all.

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "grparse/confidence.h"
#include "support/check.h"

namespace parsev1 = ai::pipestream::parse::v1;

namespace {

using grparse_test::require;
using grparse_test::require_equal;

// Line and region scores are floats; the aggregates are compared at float
// precision.
bool close(double actual, double expected) { return std::fabs(actual - expected) < 1e-6; }

grparse::OcrLine line(float confidence, std::optional<grparse::TextOrigin> origin = std::nullopt) {
  grparse::OcrLine out;
  out.text = "x";
  out.confidence = confidence;
  out.origin = origin;
  return out;
}

grparse::LayoutRegion region(const std::string& label, float confidence,
                             std::optional<float> structure = std::nullopt) {
  grparse::LayoutRegion out;
  out.label = label;
  out.confidence = confidence;
  out.structure_score = structure;
  return out;
}

void verify_page_axes() {
  grparse::OcrPage page;
  page.source = grparse::OcrPage::Source::kMerged;
  page.lines = {line(0.9F, grparse::TextOrigin::kOcr), line(0.7F, grparse::TextOrigin::kOcr),
                line(0.1F, grparse::TextOrigin::kDigitalPdf)};
  page.regions = {region("text", 0.8F), region("table", 0.6F, 0.95F), region("table", 0.4F)};
  const grparse::PageConfidence scores = grparse::page_confidence(page);
  require(scores.ocr_score.has_value() && close(*scores.ocr_score, 0.8),
          "ocr score is the mean of the recognized lines only");
  require(scores.layout_score.has_value() && close(*scores.layout_score, 0.6),
          "layout score is the mean region confidence");
  require(scores.table_score.has_value() && close(*scores.table_score, 0.95),
          "table score reads the structure score of the tables that carry one");
  require(!scores.parse_score.has_value(), "no vote, no parse score");
  require(scores.mean().has_value() && close(*scores.mean(), (0.8 + 0.6 + 0.95) / 3.0),
          "page mean is over the present axes");
  // numpy.nanquantile([0.8, 0.95, 0.6], 0.05): sorted 0.6, 0.8, 0.95;
  // position 0.1 -> 0.6 + 0.1 * 0.2.
  require(scores.low().has_value() && close(*scores.low(), 0.62), "page low is the 5th percentile");
}

void verify_digital_page_measures_nothing() {
  grparse::OcrPage page;
  page.source = grparse::OcrPage::Source::kDigitalPdf;
  page.lines = {line(1.0F), line(1.0F)};
  const grparse::PageConfidence scores = grparse::page_confidence(page);
  require(!scores.any(), "an embedded text layer without models measures no axis");
  require(!grparse::document_confidence({scores}).has_value(),
          "a document of unmeasured pages ships no report");
}

void verify_vote_is_parse_score() {
  grparse::OcrPage page;
  page.source = grparse::OcrPage::Source::kDigitalPdf;
  grparse::ConsensusVote vote;
  vote.winner_score = 0.87;
  page.vote = vote;
  const grparse::PageConfidence scores = grparse::page_confidence(page);
  require(scores.parse_score.has_value() && close(*scores.parse_score, 0.87),
          "the vote's winning score is the page's parse score");
}

void verify_document_fold() {
  grparse::PageConfidence a;
  a.ocr_score = 0.9;
  a.layout_score = 0.8;
  a.parse_score = 0.95;
  grparse::PageConfidence b;
  b.ocr_score = 0.5;
  b.parse_score = 0.55;
  grparse::PageConfidence c;  // unmeasured: contributes nothing
  const auto report = grparse::document_confidence({a, b, c});
  require(report.has_value(), "two measured pages make a report");
  require(close(report->ocr_score(), 0.7), "ocr is the page mean");
  require(close(report->layout_score(), 0.8), "layout averages the pages that measured it");
  require(!report->has_table_score(), "no table anywhere, no table score");
  // numpy.nanquantile([0.95, 0.55], 0.1): sorted 0.55, 0.95; position 0.1 ->
  // 0.55 + 0.1 * 0.4.
  require(close(report->parse_score(), 0.59), "parse is the worst-10% quantile over pages");
  const double mean_a = (0.9 + 0.8 + 0.95) / 3.0;
  const double mean_b = (0.5 + 0.55) / 2.0;
  require(close(report->mean_score(), (mean_a + mean_b) / 2.0), "mean is the mean of page means");
  // page a low: sorted 0.8, 0.9, 0.95 at position 0.1 -> 0.81; page b low:
  // sorted 0.5, 0.55 at position 0.05 -> 0.5025.
  require(close(report->low_score(), (0.81 + 0.5025) / 2.0), "low is the mean of page lows");
  require_equal(static_cast<int>(report->mean_grade()),
                static_cast<int>(parsev1::QUALITY_GRADE_FAIR), "mean grade");
  require_equal(static_cast<int>(report->low_grade()),
                static_cast<int>(parsev1::QUALITY_GRADE_FAIR), "low grade");
}

void verify_grades() {
  using grparse::quality_grade;
  require_equal(static_cast<int>(quality_grade(0.49)), static_cast<int>(parsev1::QUALITY_GRADE_POOR),
                "below 0.5 is poor");
  require_equal(static_cast<int>(quality_grade(0.5)), static_cast<int>(parsev1::QUALITY_GRADE_FAIR),
                "0.5 is fair");
  require_equal(static_cast<int>(quality_grade(0.8)), static_cast<int>(parsev1::QUALITY_GRADE_GOOD),
                "0.8 is good");
  require_equal(static_cast<int>(quality_grade(0.9)),
                static_cast<int>(parsev1::QUALITY_GRADE_EXCELLENT), "0.9 is excellent");
  require_equal(static_cast<int>(quality_grade(std::nan(""))),
                static_cast<int>(parsev1::QUALITY_GRADE_UNSPECIFIED), "nan is unspecified");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("confidence-test", "ok", {
      verify_page_axes,
      verify_digital_page_measures_nothing,
      verify_vote_is_parse_score,
      verify_document_fold,
      verify_grades,
  });
}
