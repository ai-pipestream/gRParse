// Orientation recovery driven by a fake recognizer that reads a marker
// raster: no models, deterministic, every branch of the decision visible.
// The upright raster is 4 wide by 2 tall with its marker at the top-left
// pixel; the fake reads that as five wide confident lines.  Turned a quarter
// turn it is portrait and reads as tall boxes; turned a half turn the marker
// sits bottom-right and the fake reports the classifier flipped every line.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "grparse/orientation_recovery.h"
#include "grparse/text_geometry.h"
#include "support/check.h"

namespace {

using grparse_test::require;

constexpr unsigned char kMarker = 255;
constexpr unsigned char kPoorMarker = 7;

cv::Mat upright_raster(unsigned char marker = kMarker) {
  cv::Mat raster(2, 4, CV_8UC1, cv::Scalar(0));
  raster.at<unsigned char>(0, 0) = marker;
  return raster;
}

grparse::OcrLine wide_line(int row, float confidence, bool flipped) {
  grparse::OcrLine line{"w", {{10, row}, {400, row}, {400, row + 20}, {10, row + 20}}, confidence,
                        grparse::TextOrigin::kOcr};
  line.flipped = flipped;
  return line;
}

grparse::OcrLine tall_line(int column, float confidence) {
  return grparse::OcrLine{"t", {{column, 10}, {column + 20, 10}, {column + 20, 400}, {column, 400}},
                          confidence, grparse::TextOrigin::kOcr};
}

class MarkerRecognizer final : public grparse::PageRecognizer {
 public:
  grparse::OcrPage extract_page(const cv::Mat& image) override {
    calls.fetch_add(1);
    sizes.emplace_back(image.cols, image.rows);
    grparse::OcrPage page{image.cols, image.rows, {}};
    page.source = grparse::OcrPage::Source::kOcr;
    if (image.rows > image.cols) {
      for (int line = 0; line < 5; ++line) page.lines.push_back(tall_line(30 * line, 0.7F));
      return page;
    }
    const unsigned char top_left = image.at<unsigned char>(0, 0);
    const bool upright = top_left == kMarker || top_left == kPoorMarker;
    const float confidence = top_left == kPoorMarker ? 0.3F : (upright ? 0.95F : 0.9F);
    for (int line = 0; line < 5; ++line) {
      page.lines.push_back(wide_line(30 * line, confidence, !upright));
    }
    return page;
  }

  std::atomic<int> calls{0};
  std::vector<std::pair<int, int>> sizes;
};

// cv::rotate's pixel mapping, pinned so the turn the recovery keeps means
// what the tests below say it means.
void verify_turn_raster_geometry() {
  const cv::Mat upright = upright_raster();
  const cv::Mat quarter = grparse::turn_raster(upright, 90);
  require(quarter.cols == 2 && quarter.rows == 4, "a quarter turn swaps the sides");
  require(quarter.at<unsigned char>(0, 1) == kMarker, "90 clockwise sends the top-left to the top-right");
  const cv::Mat half = grparse::turn_raster(upright, 180);
  require(half.cols == 4 && half.rows == 2 && half.at<unsigned char>(1, 3) == kMarker,
          "180 sends the top-left to the bottom-right");
  const cv::Mat three_quarters = grparse::turn_raster(upright, 270);
  require(three_quarters.cols == 2 && three_quarters.rows == 4 &&
              three_quarters.at<unsigned char>(3, 0) == kMarker,
          "270 clockwise sends the top-left to the bottom-left");
  const cv::Mat same = grparse::turn_raster(upright, 0);
  require(same.data == upright.data, "0 degrees is the raster itself");
  const cv::Mat back = grparse::turn_raster(grparse::turn_raster(upright, 90), 270);
  require(cv::countNonZero(back != upright) == 0, "a quarter turn and three quarters cancel");
  bool threw = false;
  try {
    grparse::turn_raster(upright, 45);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "a turn that is not a multiple of 90 is rejected");
}

// A raster fed in a quarter turn clockwise: the first read votes tall boxes,
// both quarter turns are tried, the one that reads upright wins, and the
// caller's raster and page are the upright ones.
void verify_quarter_turn_is_recovered() {
  MarkerRecognizer recognizer;
  cv::Mat raster = grparse::turn_raster(upright_raster(), 90);
  grparse::OcrPage page = recognizer.extract_page(raster);
  require(grparse::page_rotation_vote(page).quarter_turn, "the first read must vote a quarter turn");

  const auto outcome = grparse::recover_orientation(recognizer, {}, &raster, &page);
  require(outcome.tried == std::vector<int>{90, 270}, "a quarter-turn vote tries both quarter turns");
  require(outcome.passes == 2 && recognizer.calls.load() == 3, "two extra reads, no more");
  require(outcome.degrees == 270, "the turn that undoes a 90 clockwise feed is 270");
  require(raster.cols == 4 && raster.rows == 2 && raster.at<unsigned char>(0, 0) == kMarker,
          "the caller's raster is the upright one");
  require(page.rotation_degrees == 270 && page.width == 4 && page.height == 2,
          "the page records the turn and the upright size");
  require(page.lines.size() == 5 && !page.lines.front().flipped &&
              grparse::bounding_box(page.lines.front()).width() > 100,
          "the kept lines are the upright read's wide lines");
  require(outcome.first_confidence > 0.69F && outcome.first_confidence < 0.71F &&
              outcome.kept_confidence > 0.94F,
          "the outcome reports the confidence before and after");
}

// A raster fed in upside down: the classifier flipped every line, only the
// half turn is tried, and it wins.
void verify_half_turn_is_recovered() {
  MarkerRecognizer recognizer;
  cv::Mat raster = grparse::turn_raster(upright_raster(), 180);
  grparse::OcrPage page = recognizer.extract_page(raster);
  require(grparse::page_read_quality(page).upside_down, "the first read must say upside down");

  const auto outcome = grparse::recover_orientation(recognizer, {}, &raster, &page);
  require(outcome.tried == std::vector<int>{180}, "an upside-down read tries the half turn only");
  require(outcome.degrees == 180 && outcome.passes == 1, "the half turn wins in one pass");
  require(raster.at<unsigned char>(0, 0) == kMarker && page.rotation_degrees == 180,
          "raster and page are upright after the half turn");
}

// A poor but upright read tries every turn once and keeps itself when no
// turn reads upright: the cost is bounded and the page is not made worse.
void verify_poor_read_tries_every_turn_once_and_keeps_upright() {
  MarkerRecognizer recognizer;
  cv::Mat raster = upright_raster(kPoorMarker);
  grparse::OcrPage page = recognizer.extract_page(raster);
  require(grparse::page_read_quality(page).mean_confidence < 0.5F, "the first read must be poor");

  const auto outcome = grparse::recover_orientation(recognizer, {}, &raster, &page);
  require(outcome.tried == std::vector<int>{90, 180, 270}, "a poor read tries every turn");
  require(outcome.passes == 3 && recognizer.calls.load() == 4, "each turn is read exactly once");
  require(outcome.degrees == 0 && page.rotation_degrees == 0,
          "no turn read upright, so the first read stands");
  require(raster.cols == 4 && raster.at<unsigned char>(0, 0) == kPoorMarker,
          "the caller's raster is untouched when nothing wins");
}

// A clean upright read costs nothing, and so does a disabled recovery.
void verify_clean_and_disabled_reads_cost_nothing() {
  MarkerRecognizer recognizer;
  cv::Mat raster = upright_raster();
  grparse::OcrPage page = recognizer.extract_page(raster);
  const auto clean = grparse::recover_orientation(recognizer, {}, &raster, &page);
  require(clean.passes == 0 && clean.degrees == 0 && clean.tried.empty(),
          "a clean read triggers no re-read");
  require(recognizer.calls.load() == 1, "no extra recognition for a clean page");

  cv::Mat turned = grparse::turn_raster(upright_raster(), 90);
  grparse::OcrPage turned_page = recognizer.extract_page(turned);
  grparse::OrientationOptions off;
  off.enabled = false;
  const auto disabled = grparse::recover_orientation(recognizer, off, &turned, &turned_page);
  require(disabled.passes == 0 && turned_page.rotation_degrees == 0 && turned.rows == 4,
          "disabled recovery leaves a turned page alone");
  require(recognizer.calls.load() == 2, "disabled recovery reads nothing extra");
}

// The confidence floor is the caller's: a read the default floor calls poor
// is fine under a lower one.
void verify_confidence_floor_is_configurable() {
  MarkerRecognizer recognizer;
  cv::Mat raster = upright_raster(kPoorMarker);
  grparse::OcrPage page = recognizer.extract_page(raster);
  grparse::OrientationOptions lenient;
  lenient.confidence_floor = 0.2F;
  const auto outcome = grparse::recover_orientation(recognizer, lenient, &raster, &page);
  require(outcome.passes == 0, "a read above the floor is not re-read");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("orientation-recovery-test", {
      verify_turn_raster_geometry,
      verify_quarter_turn_is_recovered,
      verify_half_turn_is_recovered,
      verify_poor_read_tries_every_turn_once_and_keeps_upright,
      verify_clean_and_disabled_reads_cost_nothing,
      verify_confidence_floor_is_configurable,
  });
}
