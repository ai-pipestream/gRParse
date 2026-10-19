// Proves the document-level ConfidenceScores fold: which lines count as OCR,
// that an unmeasured axis stays absent rather than reading as zero, the
// upstream aggregation rules (parse the worst 10% of pages, the other axes
// page means, page mean/low folded by mean), the grade buckets, and that a
// parse with no measured page ships no report at all. ConfidenceScores.pages
// (docling 2.139) carries each page's axes, mean, low and grades under its
// 1-based number, unmeasured pages included as entries without scores.

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
  const grparse::PageConfidence scores = grparse::page_confidence(page, 1);
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
  const grparse::PageConfidence scores = grparse::page_confidence(page, 1);
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
  const grparse::PageConfidence scores = grparse::page_confidence(page, 1);
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

void verify_pages_map() {
  grparse::PageConfidence a;
  a.page_no = 1;
  a.ocr_score = 0.9;
  a.layout_score = 0.8;
  a.parse_score = 0.95;
  grparse::PageConfidence b;
  b.page_no = 2;
  b.ocr_score = 0.5;
  b.parse_score = 0.55;
  grparse::PageConfidence c;  // read but unmeasured
  c.page_no = 4;
  const auto report = grparse::document_confidence({a, b, c});
  require(report.has_value(), "two measured pages make a report");
  require_equal(report->pages_size(), 3, "every folded page has an entry");
  require(report->pages().contains(1) && report->pages().contains(2) && report->pages().contains(4),
          "pages are keyed by their 1-based number");
  require(!report->pages().contains(3), "a page the parse never read has no entry");

  const auto& first = report->pages().at(1);
  require(close(first.ocr_score(), 0.9) && close(first.layout_score(), 0.8) &&
              close(first.parse_score(), 0.95),
          "a page entry carries that page's axes");
  require(!first.has_table_score(), "an axis the page did not measure stays unset");
  require(close(first.mean_score(), (0.9 + 0.8 + 0.95) / 3.0), "page mean");
  require(close(first.low_score(), 0.81), "page low is the 5th percentile of its axes");
  require_equal(static_cast<int>(first.mean_grade()),
                static_cast<int>(parsev1::QUALITY_GRADE_GOOD), "page 1 mean grade (0.883)");
  require_equal(static_cast<int>(first.low_grade()),
                static_cast<int>(parsev1::QUALITY_GRADE_GOOD), "page 1 low grade");
  require_equal(first.pages_size(), 0, "page entries are flat");

  const auto& second = report->pages().at(2);
  require(close(second.mean_score(), 0.525) && close(second.low_score(), 0.5025), "page 2 scores");
  require_equal(static_cast<int>(second.mean_grade()),
                static_cast<int>(parsev1::QUALITY_GRADE_FAIR), "page 2 mean grade");
  require_equal(static_cast<int>(second.low_grade()),
                static_cast<int>(parsev1::QUALITY_GRADE_FAIR), "page 2 low grade");

  const auto& unmeasured = report->pages().at(4);
  require(!unmeasured.has_parse_score() && !unmeasured.has_layout_score() &&
              !unmeasured.has_table_score() && !unmeasured.has_ocr_score() &&
              !unmeasured.has_mean_score() && !unmeasured.has_low_score(),
          "an unmeasured page has no scores, not zeros");
  require_equal(static_cast<int>(unmeasured.mean_grade()),
                static_cast<int>(parsev1::QUALITY_GRADE_UNSPECIFIED), "unmeasured mean grade");
  require_equal(static_cast<int>(unmeasured.low_grade()),
                static_cast<int>(parsev1::QUALITY_GRADE_UNSPECIFIED), "unmeasured low grade");

  // The document aggregates still fold only the measured pages.
  require(close(report->ocr_score(), 0.7) && close(report->parse_score(), 0.59),
          "page entries do not change the document fold");

  // The map survives the wire.
  parsev1::ConfidenceScores wire;
  require(wire.ParseFromString(report->SerializeAsString()), "parse the serialized report");
  require_equal(wire.pages_size(), 3, "pages survive serialization");
  require(close(wire.pages().at(2).ocr_score(), 0.5), "page 2 ocr survives serialization");
}

void verify_page_number_from_caller() {
  grparse::OcrPage page;
  page.source = grparse::OcrPage::Source::kMerged;
  page.lines = {line(0.9F, grparse::TextOrigin::kOcr)};
  const grparse::PageConfidence scores = grparse::page_confidence(page, 7);
  require_equal(scores.page_no, 7, "page_confidence records the page number it was given");
  const auto report = grparse::document_confidence({scores});
  require(report.has_value() && report->pages().contains(7), "the report files it under that number");
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
      verify_pages_map,
      verify_page_number_from_caller,
      verify_grades,
  });
}
