// The fixed-shape OCR arithmetic of the Hailo backend: detector tiles,
// recognizer windows, the seam between two windows, CTC collapse and the
// page direction vote.  No device and no model: numbers in, numbers out.

#include <cmath>
#include <string>
#include <vector>

#include "../src/hailo/hailo_ocr_plan.h"
#include "support/check.h"

namespace {

using grparse::hailo::AngleScore;
using grparse::hailo::RecPlan;
using grparse::hailo::StepScore;
using grparse::hailo::TileSpan;
using grparse_test::require;
using grparse_test::require_equal;

template <typename Call>
bool throws_invalid(Call&& call) {
  try {
    call();
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

void verify_a_page_that_fits_is_one_tile() {
  const auto tiles = grparse::hailo::plan_tiles(500, 640, 64);
  require_equal(tiles.size(), size_t{1}, "a 500 pixel side is one 640 tile");
  require(tiles[0].start == 0 && tiles[0].keep_begin == 0 && tiles[0].keep_end == 500,
          "the single tile keeps the whole side");
}

// Every pixel is kept from exactly one tile, each tile lies inside the page,
// neighbours share at least the overlap, and a kept span stays inside its
// tile, away from the cut edge by half the shared span.
void verify_tiles_partition_the_side() {
  for (const int length : {641, 700, 1216, 1548, 2148, 2304, 4096}) {
    const auto tiles = grparse::hailo::plan_tiles(length, 640, 64);
    require(tiles.size() >= 2, "a side longer than the tile needs several tiles");
    require_equal(tiles.front().start, 0, "the first tile starts at the page edge");
    require_equal(tiles.back().start + 640, length, "the last tile ends at the page edge");
    int covered = 0;
    for (size_t i = 0; i < tiles.size(); ++i) {
      const TileSpan& tile = tiles[i];
      require_equal(tile.keep_begin, covered, "kept spans touch without a gap or overlap");
      require(tile.keep_begin >= tile.start && tile.keep_end <= tile.start + 640,
              "a kept span lies inside its tile");
      if (i > 0) {
        const int shared = tiles[i - 1].start + 640 - tile.start;
        require(shared >= 64, "neighbouring tiles share at least the overlap");
        require(tile.keep_begin - tile.start >= shared / 2 - 1,
                "the seam sits in the middle of the shared span");
      }
      covered = tile.keep_end;
    }
    require_equal(covered, length, "the kept spans cover the whole side");
  }
}

void verify_tile_arguments_are_checked() {
  require(throws_invalid([] { (void)grparse::hailo::plan_tiles(0, 640, 64); }),
          "a zero length is refused");
  require(throws_invalid([] { (void)grparse::hailo::plan_tiles(100, 640, 640); }),
          "an overlap as large as the tile is refused");
}

void verify_a_short_line_is_one_padded_window() {
  const RecPlan plan = grparse::hailo::plan_rec_windows(123, 320, 8);
  require_equal(plan.window_starts.size(), size_t{1}, "a 123 pixel line is one window");
  require_equal(plan.total_steps, 16, "only the steps over real pixels are decoded");
  const RecPlan exact = grparse::hailo::plan_rec_windows(320, 320, 8);
  require_equal(exact.window_starts.size(), size_t{1}, "a line as wide as the window is one window");
  require_equal(exact.total_steps, 40, "a full window decodes all of its steps");
}

void verify_a_long_line_is_overlapping_windows() {
  for (const int width : {321, 400, 640, 1000, 1801, 5000}) {
    const RecPlan plan = grparse::hailo::plan_rec_windows(width, 320, 8);
    require(plan.window_starts.size() >= 2, "a line wider than the window needs several");
    require_equal(plan.window_starts.front(), 0, "the first window starts at the line's start");
    require_equal(plan.window_starts.back() + plan.window_steps, plan.total_steps,
                  "the last window ends at the line's last step");
    for (size_t i = 1; i < plan.window_starts.size(); ++i) {
      const int shared = plan.window_starts[i - 1] + plan.window_steps - plan.window_starts[i];
      require(shared >= 8, "neighbouring windows share at least the overlap");
      require(plan.window_starts[i] > plan.window_starts[i - 1], "windows move forward");
    }
  }
}

std::vector<StepScore> steps_of(const std::vector<int>& classes) {
  std::vector<StepScore> steps;
  for (const int index : classes) steps.push_back({index, 0.5F});
  return steps;
}

std::vector<int> classes_of(const std::vector<StepScore>& steps) {
  std::vector<int> classes;
  for (const StepScore& step : steps) classes.push_back(step.index);
  return classes;
}

// Two windows of 8 steps sharing steps 4..7.  Both read blank at step 5 and
// disagree at step 6, where only the second window has the glyph whole: the
// seam is the shared blank, so the glyph is read once, from the window that
// saw all of it.
void verify_the_seam_is_a_blank_both_windows_agree_on() {
  RecPlan plan;
  plan.total_steps = 12;
  plan.window_steps = 8;
  plan.window_starts = {0, 4};
  const std::vector<std::vector<StepScore>> windows{
      steps_of({1, 0, 2, 0, 3, 0, 9, 9}),  // absolute steps 0..7; 9 is an edge misread
      steps_of({3, 0, 4, 4, 0, 5, 0, 6}),  // absolute steps 4..11
  };
  const auto line = grparse::hailo::stitch_steps(plan, windows);
  require(classes_of(line) == std::vector<int>({1, 0, 2, 0, 3, 0, 4, 4, 0, 5, 0, 6}),
          "the first window is kept up to the shared blank, the second from it");
}

void verify_the_seam_falls_back_to_the_middle() {
  RecPlan plan;
  plan.total_steps = 12;
  plan.window_steps = 8;
  plan.window_starts = {0, 4};
  const std::vector<std::vector<StepScore>> windows{
      steps_of({1, 1, 1, 1, 1, 1, 1, 1}),
      steps_of({2, 2, 2, 2, 2, 2, 2, 2}),
  };
  const auto line = grparse::hailo::stitch_steps(plan, windows);
  require(classes_of(line) == std::vector<int>({1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2}),
          "with no shared blank the seam is the middle of the shared span");
}

void verify_a_single_window_keeps_only_real_steps() {
  RecPlan plan;
  plan.total_steps = 3;
  plan.window_steps = 8;
  plan.window_starts = {0};
  const std::vector<std::vector<StepScore>> windows{steps_of({1, 0, 2, 7, 7, 7, 7, 7})};
  require(classes_of(grparse::hailo::stitch_steps(plan, windows)) == std::vector<int>({1, 0, 2}),
          "steps over the padding are not decoded");
}

void verify_stitch_checks_its_input() {
  RecPlan plan;
  plan.total_steps = 12;
  plan.window_steps = 8;
  plan.window_starts = {0, 4};
  require(throws_invalid([&] {
            const std::vector<std::vector<StepScore>> one{steps_of({0, 0, 0, 0, 0, 0, 0, 0})};
            (void)grparse::hailo::stitch_steps(plan, one);
          }),
          "a missing window is refused");
}

void verify_ctc_collapse() {
  const std::vector<std::string> keys{"#", "a", "b", " "};
  const auto steps = steps_of({0, 1, 1, 0, 1, 2, 2, 3, 9, 0});
  const auto line = grparse::hailo::ctc_collapse(steps, keys);
  require_equal(line.text, "aab ", "repeats collapse, a blank separates doubles, unknown classes drop");
  require_equal(line.char_scores.size(), size_t{4}, "one score per character");
}

void verify_argmax_steps() {
  const std::vector<float> scores{0.1F, 0.7F, 0.2F, 0.9F, 0.05F, 0.05F};
  const auto steps = grparse::hailo::argmax_steps(scores.data(), 2, 3);
  require(steps[0].index == 1 && steps[1].index == 0, "the best class of each step");
  require(steps[0].score == 0.7F && steps[1].score == 0.9F, "with its score");
}

void verify_logit_rows_score_as_probabilities() {
  // Two steps of three classes: the first decides class 2, the second blank.
  const std::vector<float> logits{0.0F, 0.0F, 2.0F, 5.0F, 1.0F, 1.0F};
  require(grparse::hailo::rows_are_logits(logits.data(), 2, 3), "values past 1 are logits");
  const std::vector<float> probabilities{0.1F, 0.7F, 0.2F, 0.9F, 0.05F, 0.05F};
  require(!grparse::hailo::rows_are_logits(probabilities.data(), 2, 3),
          "rows in [0, 1] that sum to 1 are probabilities");
  const std::vector<float> unnormalized{0.1F, 0.1F, 0.1F, 0.9F, 0.05F, 0.05F};
  require(grparse::hailo::rows_are_logits(unnormalized.data(), 2, 3),
          "a row that does not sum to 1 is not a probability row");
  const auto steps = grparse::hailo::argmax_steps(logits.data(), 2, 3, true);
  require(steps[0].index == 2 && steps[1].index == 0, "the best class is the largest logit");
  const float expected = 1.0F / (1.0F + 2.0F * std::exp(-2.0F));
  require(std::abs(steps[0].score - expected) < 1e-5F,
          "a character's score is the softmax probability of its class");
  require(steps[1].score == 0.0F, "a blank step carries no score");
}

void verify_the_page_vote_counts_only_sure_crops() {
  const std::vector<AngleScore> unsure{{1, 0.6F}, {1, 0.7F}, {0, 0.95F}};
  require_equal(grparse::hailo::vote_page_angle(unsure), 0, "unsure flips do not turn the page");
  const std::vector<AngleScore> flipped{{1, 0.95F}, {1, 0.99F}, {0, 0.95F}};
  require_equal(grparse::hailo::vote_page_angle(flipped), 1, "sure flips in the majority turn it");
  const std::vector<AngleScore> tie{{1, 0.95F}, {0, 0.95F}};
  require_equal(grparse::hailo::vote_page_angle(tie), 0, "a tie stays upright");
  require_equal(grparse::hailo::vote_page_angle({}), 0, "a page with no crops stays upright");
  require_equal(grparse::hailo::confident_angle({1, 0.89F}), 0, "an unsure crop is not flipped");
  require_equal(grparse::hailo::confident_angle({1, 0.9F}), 1, "a sure crop is");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("hailo-ocr-plan-test", "tiles, windows, seams and votes hold", {
      verify_a_page_that_fits_is_one_tile,
      verify_tiles_partition_the_side,
      verify_tile_arguments_are_checked,
      verify_a_short_line_is_one_padded_window,
      verify_a_long_line_is_overlapping_windows,
      verify_the_seam_is_a_blank_both_windows_agree_on,
      verify_the_seam_falls_back_to_the_middle,
      verify_a_single_window_keeps_only_real_steps,
      verify_stitch_checks_its_input,
      verify_ctc_collapse,
      verify_argmax_steps,
      verify_logit_rows_score_as_probabilities,
      verify_the_page_vote_counts_only_sure_crops,
  });
}
