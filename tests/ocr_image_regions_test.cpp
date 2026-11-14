// Proves how a text-layer page picks its OCR rectangles: picture regions as
// before; image placements a region of any label overlaps; a large placement
// no region overlaps (a raster figure the detector split into icons or
// missed); small, off-page and region-less small placements skipped; a figure
// and the picture over it read once; output in reading position order.

#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "grparse/ocr_image_regions.h"
#include "support/check.h"

namespace {

using grparse_test::require;

grparse::LayoutRegion region(const std::string& label, int left, int top, int right, int bottom) {
  grparse::LayoutRegion out;
  out.label = label;
  out.left = left;
  out.top = top;
  out.right = right;
  out.bottom = bottom;
  return out;
}

constexpr int kWidth = 1700;
constexpr int kHeight = 2200;

void verify_picture_regions_are_read_as_before() {
  const auto rects = grparse::ocr_image_rects({region("picture", 100, 100, 600, 500)}, {}, kWidth, kHeight, 32);
  require(rects.size() == 1 && rects[0] == cv::Rect(100, 100, 500, 400), "a picture region alone is read");
  const auto none = grparse::ocr_image_rects({region("text", 100, 100, 600, 500)}, {}, kWidth, kHeight, 32);
  require(none.empty(), "a text region with no image under it is not read");
}

void verify_a_placement_under_any_label_is_read_whole() {
  // A text region over part of a screenshot: the whole placement is read.
  const auto rects = grparse::ocr_image_rects({region("text", 300, 300, 400, 340)},
                                              {cv::Rect(200, 200, 600, 400)}, kWidth, kHeight, 32);
  require(rects.size() == 1 && rects[0] == cv::Rect(200, 200, 600, 400),
          "the placement itself is read, not its overlap with the region");
}

void verify_a_large_placement_without_a_region_is_read() {
  // Code-as-Auditor p1: a raster figure the detector saw only as icons
  // elsewhere; no region overlaps this part of it, but it covers >1% of the page.
  const auto rects = grparse::ocr_image_rects({}, {cv::Rect(880, 680, 670, 620)}, kWidth, kHeight, 32);
  require(rects.size() == 1, "a placement over 1% of the page is read without a region");
  const auto small = grparse::ocr_image_rects({}, {cv::Rect(10, 10, 60, 60)}, kWidth, kHeight, 32);
  require(small.empty(), "a small placement no region overlaps (an icon, a logo) is skipped");
  const auto thin = grparse::ocr_image_rects({region("text", 0, 0, 1700, 200)}, {cv::Rect(0, 100, 1700, 10)},
                                             kWidth, kHeight, 32);
  require(thin.empty(), "a rule thinner than the minimum side is skipped");
}

void verify_reads_clip_merge_and_order() {
  const auto off = grparse::ocr_image_rects({region("text", 1600, 2050, 1700, 2200)},
                                            {cv::Rect(1650, 2100, 400, 400)}, kWidth, kHeight, 32);
  require(off.size() == 1 && off[0] == cv::Rect(1650, 2100, 50, 100), "a placement past the edge is clipped");
  const auto merged = grparse::ocr_image_rects({region("picture", 200, 200, 800, 600)},
                                               {cv::Rect(210, 210, 600, 400)}, kWidth, kHeight, 32);
  require(merged.size() == 1 && merged[0] == cv::Rect(200, 200, 610, 410),
          "a figure and the picture region over it are one read, their union");
  const auto ordered = grparse::ocr_image_rects(
      {region("picture", 900, 1200, 1500, 1600), region("picture", 100, 300, 700, 700)}, {}, kWidth, kHeight, 32);
  require(ordered.size() == 2 && ordered[0].y == 300 && ordered[1].y == 1200, "reads go top to bottom");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("ocr-image-regions-test", "all checks passed", {
      verify_picture_regions_are_read_as_before,
      verify_a_placement_under_any_label_is_read_whole,
      verify_a_large_placement_without_a_region_is_read,
      verify_reads_clip_merge_and_order,
  });
}
