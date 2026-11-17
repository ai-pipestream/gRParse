#include "hailo_ocr_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace grparse::hailo {

std::vector<TileSpan> plan_tiles(int length, int tile, int min_overlap) {
  if (length <= 0 || tile <= 0 || min_overlap < 0 || min_overlap >= tile) {
    throw std::invalid_argument("plan_tiles: length and tile must be positive and the overlap "
                                "smaller than the tile");
  }
  if (length <= tile) return {TileSpan{0, 0, length}};
  const int stride = tile - min_overlap;
  const int count = (length - min_overlap + stride - 1) / stride;
  const int last = length - tile;
  std::vector<TileSpan> tiles(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    tiles[static_cast<size_t>(i)].start =
        static_cast<int>(static_cast<long long>(i) * last / (count - 1));
  }
  for (int i = 0; i < count; ++i) {
    TileSpan& span = tiles[static_cast<size_t>(i)];
    span.keep_begin = i == 0 ? 0 : tiles[static_cast<size_t>(i - 1)].keep_end;
    span.keep_end =
        i + 1 == count ? length : (span.start + tile + tiles[static_cast<size_t>(i + 1)].start) / 2;
  }
  return tiles;
}

RecPlan plan_rec_windows(int line_width, int window_width, int overlap_steps) {
  const int window_steps = window_width / kRecStepPixels;
  if (line_width <= 0 || window_width <= 0 || window_width % kRecStepPixels != 0 ||
      overlap_steps < 0 || overlap_steps >= window_steps) {
    throw std::invalid_argument("plan_rec_windows: the line and window must be positive, the "
                                "window a multiple of the step, and the overlap smaller than it");
  }
  RecPlan plan;
  plan.window_steps = window_steps;
  plan.total_steps = (line_width + kRecStepPixels - 1) / kRecStepPixels;
  if (plan.total_steps <= window_steps) {
    plan.window_starts = {0};
    return plan;
  }
  const int stride = window_steps - overlap_steps;
  const int count = (plan.total_steps - overlap_steps + stride - 1) / stride;
  const int last = plan.total_steps - window_steps;
  plan.window_starts.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    plan.window_starts.push_back(static_cast<int>(static_cast<long long>(i) * last / (count - 1)));
  }
  return plan;
}

std::vector<StepScore> argmax_steps(const float* scores, size_t steps, size_t classes,
                                    bool logits) {
  std::vector<StepScore> best(steps);
  for (size_t step = 0; step < steps; ++step) {
    const float* row = scores + step * classes;
    const float* top = std::max_element(row, row + classes);
    const int index = static_cast<int>(top - row);
    float score = *top;
    if (logits) {
      score = 0.0F;
      if (index != 0) {
        float total = 0.0F;
        for (size_t i = 0; i < classes; ++i) total += std::exp(row[i] - *top);
        score = 1.0F / total;
      }
    }
    best[step] = StepScore{index, score};
  }
  return best;
}

bool rows_are_logits(const float* scores, size_t steps, size_t classes) {
  for (size_t step = 0; step < steps; ++step) {
    const float* row = scores + step * classes;
    float total = 0.0F;
    for (size_t i = 0; i < classes; ++i) {
      if (row[i] < -0.01F || row[i] > 1.01F) return true;
      total += row[i];
    }
    if (std::abs(total - 1.0F) > 0.25F) return true;
  }
  return false;
}

namespace {

// The first step window `next` contributes, given that `prev` starts at
// prev_start: a step both read as blank, nearest the middle of what they
// share.
int seam_between(const std::vector<StepScore>& prev, int prev_start,
                 const std::vector<StepScore>& next, int next_start, int window_steps) {
  const int shared_begin = next_start;
  const int shared_end = prev_start + window_steps;
  const int middle = (shared_begin + shared_end) / 2;
  int seam = middle;
  int best_distance = -1;
  for (int step = shared_begin; step < shared_end; ++step) {
    const bool both_blank = prev[static_cast<size_t>(step - prev_start)].index == 0 &&
                            next[static_cast<size_t>(step - next_start)].index == 0;
    if (!both_blank) continue;
    const int distance = std::abs(step - middle);
    if (best_distance < 0 || distance < best_distance) {
      best_distance = distance;
      seam = step;
    }
  }
  return seam;
}

}  // namespace

std::vector<StepScore> stitch_steps(const RecPlan& plan,
                                    std::span<const std::vector<StepScore>> windows) {
  if (windows.size() != plan.window_starts.size()) {
    throw std::invalid_argument("stitch_steps: one step list per planned window");
  }
  for (const auto& window : windows) {
    if (window.size() != static_cast<size_t>(plan.window_steps)) {
      throw std::invalid_argument("stitch_steps: a window has the wrong number of steps");
    }
  }
  std::vector<StepScore> line;
  line.reserve(static_cast<size_t>(plan.total_steps));
  int from = 0;
  for (size_t i = 0; i < windows.size(); ++i) {
    const int start = plan.window_starts[i];
    const int until =
        i + 1 == windows.size()
            ? std::min(plan.total_steps, start + plan.window_steps)
            : seam_between(windows[i], start, windows[i + 1], plan.window_starts[i + 1],
                           plan.window_steps);
    for (int step = from; step < until; ++step) {
      line.push_back(windows[i][static_cast<size_t>(step - start)]);
    }
    from = until;
  }
  return line;
}

DecodedLine ctc_collapse(std::span<const StepScore> steps, std::span<const std::string> keys) {
  DecodedLine line;
  int previous = 0;
  for (size_t i = 0; i < steps.size(); ++i) {
    const int index = steps[i].index;
    const bool repeat = i > 0 && index == previous;
    if (index > 0 && static_cast<size_t>(index) < keys.size() && !repeat) {
      line.text += keys[static_cast<size_t>(index)];
      line.char_scores.push_back(steps[i].score);
    }
    previous = index;
  }
  return line;
}

int vote_page_angle(std::span<const AngleScore> angles) {
  size_t flipped = 0;
  size_t upright = 0;
  for (const AngleScore& angle : angles) {
    if (angle.score < kFlipScore) continue;
    if (angle.index == 1) {
      ++flipped;
    } else {
      ++upright;
    }
  }
  return flipped > upright ? 1 : 0;
}

int confident_angle(const AngleScore& angle) {
  return angle.index == 1 && angle.score >= kFlipScore ? 1 : 0;
}

}  // namespace grparse::hailo
