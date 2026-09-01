#include "grparse/orientation_recovery.h"

#include <stdexcept>
#include <utility>

#include <opencv2/core.hpp>

#include "grparse/text_geometry.h"

namespace grparse {

cv::Mat turn_raster(const cv::Mat& raster, int degrees) {
  cv::Mat turned;
  switch (degrees) {
    case 0:
      return raster;
    case 90:
      cv::rotate(raster, turned, cv::ROTATE_90_CLOCKWISE);
      return turned;
    case 180:
      cv::rotate(raster, turned, cv::ROTATE_180);
      return turned;
    case 270:
      cv::rotate(raster, turned, cv::ROTATE_90_COUNTERCLOCKWISE);
      return turned;
    default:
      throw std::invalid_argument("A raster turn must be 0, 90, 180 or 270 degrees");
  }
}

OrientationOutcome recover_orientation(PageRecognizer& recognizer, const OrientationOptions& options,
                                       cv::Mat* raster, OcrPage* page) {
  OrientationOutcome outcome;
  if (!options.enabled || raster == nullptr || page == nullptr || raster->empty()) return outcome;
  const ReadQuality quality = page_read_quality(*page);
  outcome.first_confidence = quality.mean_confidence;
  outcome.kept_confidence = quality.mean_confidence;
  const std::vector<int> candidates = rotation_candidates(
      page_rotation_vote(*page), quality, options.confidence_floor, options.minimum_lines);
  if (candidates.empty()) return outcome;

  ReadScore best = score_read(*page);
  cv::Mat best_raster;
  OcrPage best_page;
  int best_degrees = 0;
  for (const int degrees : candidates) {
    cv::Mat turned = turn_raster(*raster, degrees);
    OcrPage read = recognizer.extract_page(turned);
    ++outcome.passes;
    outcome.tried.push_back(degrees);
    const ReadScore score = score_read(read);
    if (score > best) {
      best = score;
      best_raster = std::move(turned);
      best_page = std::move(read);
      best_degrees = degrees;
    }
  }
  if (best_degrees == 0) return outcome;

  best_page.width = best_raster.cols;
  best_page.height = best_raster.rows;
  best_page.rotation_degrees = best_degrees;
  *raster = std::move(best_raster);
  *page = std::move(best_page);
  outcome.degrees = best_degrees;
  outcome.kept_confidence = best.mean_confidence;
  return outcome;
}

}  // namespace grparse
