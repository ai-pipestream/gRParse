// The OCR detector's input bucketing (#249): the longest side of the
// detector input comes from a short ladder of sizes capped by
// GRPARSE_OCR_MAX_SIDE, and each page is letterboxed onto it, so pages of
// many sizes share a few input shapes and the accelerated providers'
// per-shape work (cuDNN algorithm choice, OpenVINO kernel compiles) is paid
// a few times instead of once per page size.  Pure functions: no model.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <set>
#include <utility>
#include <vector>

#include "grparse/ocr_engine.h"
#include "support/check.h"

namespace {

using grparse::ocr_detect_geometry;
using grparse::ocr_detector_side;
using grparse_test::require;
using grparse_test::require_equal;

constexpr int kCaps[] = {32, 100, 640, 960, 1000, 1500, 2048, 3000, 16384};

int cap_side(int max_side_len) { return std::max(32, max_side_len / 32 * 32); }

// The sizes a cap allows: the ladder below it, then the cap itself.
std::vector<int> allowed_sides(int max_side_len) {
  std::vector<int> sides;
  for (const int side : grparse::kOcrDetectorLadder) {
    if (side < cap_side(max_side_len)) sides.push_back(side);
  }
  sides.push_back(cap_side(max_side_len));
  return sides;
}

// A sweep of widths lands on the ladder, never above the cap, never smaller
// for a larger page, and reaches every size the cap allows.
void verify_sweep_lands_on_the_ladder() {
  for (const int cap : kCaps) {
    const std::vector<int> allowed = allowed_sides(cap);
    std::set<int> reached;
    int previous = 0;
    for (int width = 1; width <= 20000; ++width) {
      const int side = ocr_detector_side(width, cap);
      const std::string where = std::format("width {} under cap {}", width, cap);
      require(std::ranges::find(allowed, side) != allowed.end(),
              where + " left the ladder: " + std::to_string(side));
      require(side <= std::max(32, cap), where + " exceeds max_side_len: " + std::to_string(side));
      require(side % 32 == 0, where + " is not a multiple of 32: " + std::to_string(side));
      require(side >= previous, where + " is smaller than for a narrower page");
      previous = side;
      reached.insert(side);
    }
    require_equal(reached.size(), allowed.size(),
                  std::format("every allowed size is reached under cap {}", cap));
    require_equal(ocr_detector_side(20000, cap), cap_side(cap),
                  "a page past the cap is detected at the cap");
  }
}

// Between two sizes the nearer by ratio wins; below the first, the first.
void verify_nearest_size_by_ratio() {
  require_equal(ocr_detector_side(1, 2048), 960, "a tiny page takes the smallest size");
  require_equal(ocr_detector_side(960, 2048), 960, "a size maps to itself");
  require_equal(ocr_detector_side(1100, 2048), 960, "1100 is nearer 960 than 1280");
  require_equal(ocr_detector_side(1200, 2048), 1280, "1200 is nearer 1280 than 960");
  // A US Letter page rendered at 2x with the default 50 px padding.
  require_equal(ocr_detector_side(1684, 2048), 1600, "1684 is nearer 1600 than 2048");
  require_equal(ocr_detector_side(1900, 2048), 2048, "1900 is nearer 2048 than 1600");
  require_equal(ocr_detector_side(2148, 2048), 2048, "the default cap holds a large page");
  require_equal(ocr_detector_side(1400, 1500), 1472,
                "a cap off the ladder becomes its top size, rounded down to 32");
  require_equal(ocr_detector_side(5000, 640), 640, "a cap below the ladder is the only size");
}

// The letterbox geometry: inputs are multiples of 32 led by the ladder size,
// the canvas holds the padded page, and both axes scale alike.
void verify_letterbox_geometry() {
  for (const int cap : {640, 1500, 2048, 4096}) {
    for (const int padding : {0, 50}) {
      for (int width = 1; width <= 6000; width += 53) {
        for (int height = 1; height <= 6000; height += 61) {
          const auto geometry = ocr_detect_geometry(width, height, padding, cap);
          const std::string where =
              std::format("{}x{} page, padding {}, cap {}", width, height, padding, cap);
          const int padded_width = width + 2 * padding;
          const int padded_height = height + 2 * padding;
          require(geometry.input_width % 32 == 0 && geometry.input_height % 32 == 0,
                  where + ": the detector input is not a multiple of 32");
          require_equal(std::max(geometry.input_width, geometry.input_height),
                        ocr_detector_side(std::max(padded_width, padded_height), cap),
                        where + ": the input's longest side is the ladder size");
          require(geometry.input_width <= cap_side(cap) && geometry.input_height <= cap_side(cap),
                  where + ": the input exceeds max_side_len");
          require(geometry.canvas_width >= padded_width && geometry.canvas_height >= padded_height,
                  where + ": the canvas must hold the padded page");
          // The short axis, scaled by the long axis's factor, lands on the
          // input within a pixel: the resize does not distort the page.
          const bool wide = padded_width >= padded_height;
          const double scale = wide ? static_cast<double>(geometry.input_width) /
                                          geometry.canvas_width
                                    : static_cast<double>(geometry.input_height) /
                                          geometry.canvas_height;
          const double short_canvas = wide ? geometry.canvas_height : geometry.canvas_width;
          const double short_input = wide ? geometry.input_height : geometry.input_width;
          require(std::abs(short_canvas * scale - short_input) <= 1.0,
                  where + ": the resize distorts the page");
          require(scale <= 1.0 + 1e-9 || std::max(padded_width, padded_height) >
                                             ocr_detector_side(1, cap),
                  where + ": a page below the first size is padded, never scaled up");
        }
      }
    }
  }
  // A page below the first size is padded out to it, not scaled.
  const auto small = ocr_detect_geometry(400, 300, 50, 2048);
  require(small.input_width == 960 && small.input_height == 416 && small.canvas_width == 960 &&
              small.canvas_height == 416,
          std::format("a small page is padded, not scaled (got input {}x{}, canvas {}x{})",
                      small.input_width, small.input_height, small.canvas_width,
                      small.canvas_height));
}

// The point of the ladder: one paper size rendered at forty-one scales
// between 1x and 3x takes a handful of input shapes, not forty-one.
void verify_render_scales_share_shapes() {
  for (const auto& [points_wide, points_high] :
       {std::pair{612.0, 792.0}, std::pair{595.0, 842.0}, std::pair{792.0, 612.0}}) {
    std::set<std::pair<int, int>> shapes;
    for (int step = 20; step <= 60; ++step) {
      const double scale = step / 20.0;
      const auto geometry =
          ocr_detect_geometry(static_cast<int>(std::lround(points_wide * scale)),
                              static_cast<int>(std::lround(points_high * scale)), 50, 2048);
      shapes.emplace(geometry.input_width, geometry.input_height);
    }
    require(shapes.size() <= 8,
            std::format("{}x{} pt pages took {} detector shapes across render scales",
                        points_wide, points_high, shapes.size()));
  }
}

}  // namespace

int main() {
  return grparse_test::run_test_main("ocr-engine-test", "all checks passed",
                                     {
                                         verify_sweep_lands_on_the_ladder,
                                         verify_nearest_size_by_ratio,
                                         verify_letterbox_geometry,
                                         verify_render_scales_share_shapes,
                                     });
}
