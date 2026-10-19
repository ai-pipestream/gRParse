#include "grparse/confidence.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

std::optional<double> mean_of(const std::vector<double>& values) {
  if (values.empty()) return std::nullopt;
  return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

// numpy.quantile with linear interpolation over the sorted sample.
std::optional<double> quantile_of(std::vector<double> values, double q) {
  if (values.empty()) return std::nullopt;
  std::ranges::sort(values);
  const double position = q * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const std::size_t upper = std::min(lower + 1, values.size() - 1);
  const double fraction = position - static_cast<double>(lower);
  return values[lower] + fraction * (values[upper] - values[lower]);
}

std::vector<double> present(const PageConfidence& page) {
  std::vector<double> values;
  for (const auto& axis : {page.ocr_score, page.table_score, page.layout_score, page.parse_score}) {
    if (axis.has_value()) values.push_back(*axis);
  }
  return values;
}

TextOrigin origin_of(const OcrPage& page, const OcrLine& line) {
  if (line.origin.has_value()) return *line.origin;
  return page.source == OcrPage::Source::kDigitalPdf ? TextOrigin::kDigitalPdf : TextOrigin::kOcr;
}

}  // namespace

bool PageConfidence::any() const {
  return parse_score.has_value() || layout_score.has_value() || table_score.has_value() ||
         ocr_score.has_value();
}

std::optional<double> PageConfidence::mean() const { return mean_of(present(*this)); }

std::optional<double> PageConfidence::low() const { return quantile_of(present(*this), 0.05); }

PageConfidence page_confidence(const OcrPage& page, int page_no) {
  PageConfidence confidence;
  confidence.page_no = page_no;
  std::vector<double> ocr;
  for (const OcrLine& line : page.lines) {
    if (line.confidence.has_value() && origin_of(page, line) == TextOrigin::kOcr) {
      ocr.push_back(static_cast<double>(*line.confidence));
    }
  }
  confidence.ocr_score = mean_of(ocr);

  std::vector<double> layout;
  std::vector<double> table;
  for (const LayoutRegion& region : page.regions) {
    layout.push_back(static_cast<double>(region.confidence));
    if (region.structure_score.has_value()) {
      table.push_back(static_cast<double>(*region.structure_score));
    }
  }
  confidence.layout_score = mean_of(layout);
  confidence.table_score = mean_of(table);

  if (page.vote.has_value()) confidence.parse_score = page.vote->winner_score;
  return confidence;
}

pipestream::parse::v1::QualityGrade quality_grade(double score) {
  if (std::isnan(score)) return pipestream::parse::v1::QUALITY_GRADE_UNSPECIFIED;
  if (score < 0.5) return pipestream::parse::v1::QUALITY_GRADE_POOR;
  if (score < 0.8) return pipestream::parse::v1::QUALITY_GRADE_FAIR;
  if (score < 0.9) return pipestream::parse::v1::QUALITY_GRADE_GOOD;
  return pipestream::parse::v1::QUALITY_GRADE_EXCELLENT;
}

pipestream::parse::v1::ConfidenceScores page_scores(const PageConfidence& page) {
  pipestream::parse::v1::ConfidenceScores scores;
  if (page.parse_score) scores.set_parse_score(*page.parse_score);
  if (page.layout_score) scores.set_layout_score(*page.layout_score);
  if (page.table_score) scores.set_table_score(*page.table_score);
  if (page.ocr_score) scores.set_ocr_score(*page.ocr_score);
  if (const auto mean = page.mean()) {
    scores.set_mean_score(*mean);
    scores.set_mean_grade(quality_grade(*mean));
  }
  if (const auto low = page.low()) {
    scores.set_low_score(*low);
    scores.set_low_grade(quality_grade(*low));
  }
  return scores;
}

std::optional<pipestream::parse::v1::ConfidenceScores> document_confidence(
    const std::vector<PageConfidence>& pages) {
  std::vector<double> parse;
  std::vector<double> layout;
  std::vector<double> table;
  std::vector<double> ocr;
  std::vector<double> means;
  std::vector<double> lows;
  for (const PageConfidence& page : pages) {
    if (!page.any()) continue;
    if (page.parse_score) parse.push_back(*page.parse_score);
    if (page.layout_score) layout.push_back(*page.layout_score);
    if (page.table_score) table.push_back(*page.table_score);
    if (page.ocr_score) ocr.push_back(*page.ocr_score);
    means.push_back(*page.mean());
    lows.push_back(*page.low());
  }
  if (means.empty()) return std::nullopt;

  pipestream::parse::v1::ConfidenceScores scores;
  if (const auto value = quantile_of(parse, 0.1)) scores.set_parse_score(*value);
  if (const auto value = mean_of(layout)) scores.set_layout_score(*value);
  if (const auto value = mean_of(table)) scores.set_table_score(*value);
  if (const auto value = mean_of(ocr)) scores.set_ocr_score(*value);
  const double mean_score = *mean_of(means);
  const double low_score = *mean_of(lows);
  scores.set_mean_score(mean_score);
  scores.set_low_score(low_score);
  scores.set_mean_grade(quality_grade(mean_score));
  scores.set_low_grade(quality_grade(low_score));
  auto& by_page = *scores.mutable_pages();
  for (const PageConfidence& page : pages) {
    if (page.page_no < 1) continue;  // not a document page; no key to file it under
    by_page[page.page_no] = page_scores(page);
  }
  return scores;
}

}  // namespace grparse
