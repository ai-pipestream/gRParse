// Proves how native TensorRT picks an optimization profile
// (src/tensorrt/shape_profiles.h) and that the OCR nets' profiles
// (src/tensorrt/ocr_profiles.h) cover every input RapidOCR can produce.  No
// TensorRT or GPU is involved: the rules are functions of shapes.

#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../src/tensorrt/ocr_profiles.h"
#include "../src/tensorrt/shape_profiles.h"
#include "grparse_inference_model.h"
#include "support/check.h"

namespace {

using grparse::ShapeProfile;
using grparse::ShapeRange;
using grparse::trt::choose_profile;
using grparse::trt::ProfileChoice;
using grparse_test::require;
using grparse_test::require_equal;

using Shapes = std::vector<std::vector<int64_t>>;

template <typename Call>
bool throws_runtime_error(Call&& call) {
  try {
    call();
  } catch (const std::runtime_error&) {
    return true;
  } catch (const std::logic_error&) {
    return false;
  }
  return false;
}

ShapeProfile line_profile(int64_t rows, int64_t width) {
  ShapeProfile profile;
  profile.inputs.push_back(ShapeRange{{1, 3, 48, 16}, {rows, 3, 48, width}, {rows, 3, 48, width}});
  return profile;
}

void verify_smallest_covering_profile_wins() {
  const std::vector<ShapeProfile> profiles{line_profile(16, 64), line_profile(16, 320),
                                           line_profile(16, 2560)};
  require_equal(choose_profile(profiles, Shapes{{4, 3, 48, 64}}, "t").profile, size_t{0},
                "a width on the first bucket takes it");
  require_equal(choose_profile(profiles, Shapes{{4, 3, 48, 65}}, "t").profile, size_t{1},
                "one pixel wider takes the next");
  require_equal(choose_profile(profiles, Shapes{{16, 3, 48, 2560}}, "t").profile, size_t{2},
                "the widest listed line takes the last");
  // Listing order does not decide: the smallest covering profile does.
  const std::vector<ShapeProfile> reversed{line_profile(16, 2560), line_profile(16, 64)};
  require_equal(choose_profile(reversed, Shapes{{1, 3, 48, 32}}, "t").profile, size_t{1},
                "the smaller profile wins wherever it is listed");
}

void verify_rows_split_and_bounds() {
  const std::vector<ShapeProfile> profiles{line_profile(16, 320)};
  const ProfileChoice choice = choose_profile(profiles, Shapes{{40, 3, 48, 300}}, "t");
  require_equal(choice.max_rows, int64_t{16}, "forty rows run sixteen at a time");
  require(throws_runtime_error([&] { choose_profile(profiles, Shapes{{1, 3, 48, 321}}, "t"); }),
          "a line past every profile is refused");
  require(throws_runtime_error([&] { choose_profile(profiles, Shapes{{1, 3, 48, 8}}, "t"); }),
          "a line under the smallest width is refused");
  require(throws_runtime_error([&] { choose_profile(profiles, Shapes{{1, 3, 32, 300}}, "t"); }),
          "a fixed dimension that differs is refused");
  require(throws_runtime_error([&] { choose_profile(profiles, Shapes{{1, 3, 48}}, "t"); }),
          "a different rank is refused");
  try {
    choose_profile(profiles, Shapes{{1, 3, 48, 9000}}, "OCR recognition");
  } catch (const std::runtime_error& error) {
    const std::string text = error.what();
    require(text.find("OCR recognition") != std::string::npos &&
                text.find("1x3x48x9000") != std::string::npos,
            "the refusal names the model and the shape");
  }
}

void verify_multi_input_profiles() {
  const std::vector<ShapeProfile> profiles{
      grparse::static_profile({{1, 3, 640, 640}, {1, 2}})};
  require_equal(choose_profile(profiles, Shapes{{1, 3, 640, 640}, {1, 2}}, "layout").profile,
                size_t{0}, "the static shapes are served");
  require(throws_runtime_error(
              [&] { choose_profile(profiles, Shapes{{1, 3, 640, 640}, {1, 3}}, "layout"); }),
          "a second input of the wrong shape is refused");
  require(throws_runtime_error([&] { choose_profile(profiles, Shapes{{1, 3, 640, 640}}, "layout"); }),
          "a missing input is refused");
}

void verify_profile_validation() {
  std::vector<ShapeProfile> profiles{line_profile(16, 320)};
  grparse::trt::validate_profiles(profiles, 1, "t");
  bool refused = false;
  try {
    grparse::trt::validate_profiles(profiles, 2, "t");
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  require(refused, "a profile with too few inputs is refused");
  profiles[0].inputs[0].opt = {32, 3, 48, 320};  // opt above max
  refused = false;
  try {
    grparse::trt::validate_profiles(profiles, 1, "t");
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  require(refused, "opt above max is refused");
}

// Every size RapidOCR can hand the detector at the default settings (each
// side a multiple of 32 up to 2144) is inside the one detector profile,
// which is tuned for an upright letter page.
void verify_detector_profile() {
  const int limit = grparse::trt::detector_side_limit(2048, 50);
  require_equal(limit, 2144, "the default side limit is 2144");
  const std::vector<ShapeProfile> profiles = grparse::trt::detector_profiles(limit);
  require_equal(profiles.size(), size_t{1}, "one range profile serves the detector");
  const ShapeRange& range = profiles[0].inputs[0];
  require(range.min == std::vector<int64_t>({1, 3, 32, 32}), "from 32 x 32");
  require(range.opt == std::vector<int64_t>({1, 3, 2144, 1664}), "tuned for a letter page");
  require(range.max == std::vector<int64_t>({1, 3, 2144, 2144}), "up to the side limit squared");
  for (int height = 32; height <= limit; height += 32) {
    for (int width = 32; width <= limit; width += 32) {
      require_equal(choose_profile(profiles, Shapes{{1, 3, height, width}}, "OCR detection").max_rows,
                    int64_t{1}, std::format("{}x{} is covered, one page per call", width, height));
    }
  }
  require(throws_runtime_error(
              [&] { choose_profile(profiles, Shapes{{1, 3, limit + 32, 640}}, "OCR detection"); }),
          "a side past the limit is refused");
}

// The profile follows GRPARSE_OCR_MAX_SIDE and GRPARSE_OCR_PADDING, so no
// configured size falls outside it and a small one tunes for itself.
void verify_detector_profile_follows_side_limit() {
  const ShapeRange large = grparse::trt::detector_profiles(4096)[0].inputs[0];
  require(large.max == std::vector<int64_t>({1, 3, 4096, 4096}), "a larger limit widens the range");
  require(large.opt == std::vector<int64_t>({1, 3, 2144, 1664}), "and keeps the letter-page tuning");
  const ShapeRange small = grparse::trt::detector_profiles(960)[0].inputs[0];
  require(small.max == std::vector<int64_t>({1, 3, 960, 960}) && small.opt == small.max,
          "a smaller limit clips the range and the tuned size");
  grparse::trt::validate_profiles(grparse::trt::detector_profiles(32), 1, "OCR detection");
  require_equal(grparse::trt::detector_side_limit(960, 0), 960, "a multiple of 32 stays");
  require_equal(grparse::trt::detector_side_limit(1000, 50), 1088, "1100 rounds down to 1088");
  require_equal(grparse::trt::detector_side_limit(8, 0), 32, "never under one 32-pixel cell");
}

void verify_recognizer_and_direction_profiles() {
  const std::vector<ShapeProfile> profiles = grparse::trt::recognizer_profiles(16);
  require_equal(profiles.size(), size_t{2}, "batches of lines, and single wide lines");
  grparse::trt::validate_profiles(profiles, 1, "OCR recognition");
  for (int width = grparse::trt::kRecMinWidth; width <= grparse::trt::kRecMaxWidth; ++width) {
    const ProfileChoice choice = choose_profile(profiles, Shapes{{16, 3, 48, width}}, "rec");
    require(profiles[choice.profile].inputs[0].max[3] >= width, std::format("width {} is covered", width));
    require_equal(choice.max_rows, width <= 2560 ? int64_t{16} : int64_t{1},
                  std::format("width {} runs at its profile's batch", width));
  }
  require(throws_runtime_error([&] {
            choose_profile(profiles, Shapes{{1, 3, 48, grparse::trt::kRecMaxWidth + 1}}, "rec");
          }),
          "a line past the widest profile is refused (the patched net resizes it first)");
  require(throws_runtime_error([&] {
            choose_profile(profiles, Shapes{{1, 3, 48, grparse::trt::kRecMinWidth - 1}}, "rec");
          }),
          "a line under the narrowest is refused (the patched net pads it first)");
  require_equal(grparse::trt::recognizer_profiles(1)[0].inputs[0].max[0], int64_t{1},
                "GRPARSE_OCR_BATCH=1 builds single-line profiles");
  require_equal(grparse::trt::recognizer_profiles(64)[0].inputs[0].opt[0], int64_t{64},
                "a larger batch is what the profile is tuned for");
  const std::vector<ShapeProfile> direction = grparse::trt::direction_profiles(16);
  require_equal(direction.size(), size_t{1}, "the direction net has one shape");
  require(direction[0].inputs[0].max == std::vector<int64_t>({16, 3, 48, 192}),
          "sixteen crops of 48 by 192");
  require_equal(choose_profile(direction, Shapes{{1, 3, 48, 192}}, "cls").profile, size_t{0},
                "a single crop is served");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("tensorrt-profiles-test", "all checks passed", {
      verify_smallest_covering_profile_wins,
      verify_rows_split_and_bounds,
      verify_multi_input_profiles,
      verify_profile_validation,
      verify_detector_profile,
      verify_detector_profile_follows_side_limit,
      verify_recognizer_and_direction_profiles,
  });
}
