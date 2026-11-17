#pragma once

// The shape arithmetic of OCR on a device that only runs fixed input shapes:
// how a page is cut into detector tiles, how a line is cut into recognizer
// windows, and how the per-window time steps join back into one line.  Pure
// functions over numbers, with no device, model or image type, so the policy
// is tested on every platform (tests/hailo_ocr_plan_test.cpp).

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace grparse::hailo {

// One tile along one axis: it covers [start, start + tile) of the page, and
// the page keeps [keep_begin, keep_end) from it.  The kept spans of
// consecutive tiles touch and never overlap, so every page pixel comes from
// exactly one tile, taken from the part of that tile farthest from its edges.
struct TileSpan {
  int start = 0;
  int keep_begin = 0;
  int keep_end = 0;
};

// Tiles of length `tile` covering [0, length) with at least `min_overlap`
// shared between neighbours.  A length that fits one tile gets one tile at 0
// (the caller pads the rest); otherwise the last tile ends at `length`, so
// no tile is padded.  Throws std::invalid_argument for a non-positive length
// or tile, or an overlap that is negative or not smaller than the tile.
std::vector<TileSpan> plan_tiles(int length, int tile, int min_overlap);

// The recognizer emits one time step per kRecStepPixels columns of its
// 48-pixel-high input.
inline constexpr int kRecStepPixels = 8;
inline constexpr int kRecHeight = 48;

// How one line of `line_width` pixels (at height 48) is read by a recognizer
// whose input is `window_width` wide.  All positions are in time steps.
// A line no wider than the window is one window padded on the right, and
// only the steps over real pixels are decoded.  A wider line is read as
// windows that share at least `overlap_steps` steps with their neighbour;
// the last window ends at the line's last step.
struct RecPlan {
  int total_steps = 0;           // time steps over the real line
  int window_steps = 0;          // time steps one window yields
  std::vector<int> window_starts;  // first step of each window
};

RecPlan plan_rec_windows(int line_width, int window_width, int overlap_steps);

// The class a time step decided on and how sure it was.
struct StepScore {
  int index = 0;
  float score = 0.0F;
};

// The best class of each of `steps` rows of `classes` scores.  With `logits`
// the rows are unnormalized (a recognizer HEF that ends before its softmax)
// and a step's score is the softmax probability of its best class, so a
// character's confidence means the same either way; a blank step's score is
// not computed (0), since no character carries it.
std::vector<StepScore> argmax_steps(const float* scores, size_t steps, size_t classes,
                                    bool logits = false);

// True when the rows are not probabilities: a value outside [0, 1] or a row
// that does not sum to about 1.
bool rows_are_logits(const float* scores, size_t steps, size_t classes);

// Joins per-window steps into the line's `total_steps` steps.  Between two
// neighbours the seam is the step, inside their shared span and nearest its
// middle, where both windows read blank (class 0): a glyph left of it was
// emitted left of it by both, so none is doubled or dropped.  When the two
// never agree on a blank the seam is the middle of the shared span.
// `windows[i]` holds window i's `window_steps` steps.
std::vector<StepScore> stitch_steps(const RecPlan& plan,
                                    std::span<const std::vector<StepScore>> windows);

struct DecodedLine {
  std::string text;
  std::vector<float> char_scores;
};

// Greedy CTC: class 0 is blank, a class repeated on consecutive steps is one
// character, and a class past the end of `keys` is dropped.  `keys[0]` is
// the blank's placeholder, as RapidOCR lays its dictionary out.
DecodedLine ctc_collapse(std::span<const StepScore> steps, std::span<const std::string> keys);

// The direction net's verdict for one crop: 0 upright, 1 upside down.
struct AngleScore {
  int index = 0;
  float score = 0.0F;
};

// A crop counts toward the page vote only when the net is at least this sure.
inline constexpr float kFlipScore = 0.9F;

// The page's direction by vote of the confident crops; ties and pages with
// no confident crop are upright.  Same rule as the CPU path's angle vote.
int vote_page_angle(std::span<const AngleScore> angles);

// Without the page vote: a crop is flipped only when the net is sure.
int confident_angle(const AngleScore& angle);

}  // namespace grparse::hailo
