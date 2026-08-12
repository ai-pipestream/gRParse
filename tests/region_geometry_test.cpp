// Exercises the single region-binding rule (highest-confidence region
// containing the line's box center) and the raster clipping/cropping helpers
// that table structure, figure classification, and barcode decode all rely on.
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"
#include "grparse/region_geometry.h"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

grparse::OcrLine make_line(int left, int top, int right, int bottom) {
  return grparse::OcrLine{"line",
                          {{left, top}, {right, top}, {right, bottom}, {left, bottom}},
                          std::nullopt,
                          grparse::TextOrigin::kOcr};
}

grparse::LayoutRegion make_region(std::string label, float confidence, int left, int top,
                                  int right, int bottom) {
  grparse::LayoutRegion region;
  region.label = std::move(label);
  region.confidence = confidence;
  region.left = left;
  region.top = top;
  region.right = right;
  region.bottom = bottom;
  return region;
}

void verify_region_binding() {
  grparse::OcrPage page;
  page.width = 1000;
  page.height = 1000;
  page.regions = {make_region("table", 0.9F, 0, 0, 100, 100),
                  make_region("figure", 0.5F, 50, 50, 150, 150),
                  make_region("text", 0.7F, 400, 400, 500, 500)};

  // Center (75, 75) lies inside both the table and the figure; confidence wins.
  const auto* both = grparse::region_for_line(page, make_line(60, 60, 90, 90));
  require(both != nullptr && both->label == "table", "highest-confidence containing region wins");

  // Center (125, 125) lies only inside the figure.
  const auto* figure_only = grparse::region_for_line(page, make_line(110, 110, 140, 140));
  require(figure_only != nullptr && figure_only->label == "figure",
          "sole containing region binds");

  // The rule is center containment: a line overlapping the table's corner but
  // centered outside binds to nothing.
  const auto* overlap_only = grparse::region_for_line(page, make_line(90, 90, 300, 300));
  require(overlap_only == nullptr, "overlap without center containment must not bind");

  // Region edges are inclusive for the center point.
  const auto* on_edge = grparse::region_for_line(page, make_line(90, 90, 110, 110));
  require(on_edge != nullptr && on_edge->label == "table", "center on the region edge binds");

  require(grparse::region_for_line(page, make_line(800, 800, 900, 900)) == nullptr,
          "line outside every region binds to nothing");

  grparse::OcrLine degenerate;
  require(grparse::region_for_line(page, degenerate) == nullptr,
          "a line without a polygon binds to nothing");

  grparse::OcrPage bare;
  require(grparse::region_for_line(bare, make_line(0, 0, 10, 10)) == nullptr,
          "a page without regions binds nothing");
}

void verify_clip_region() {
  const auto inside = grparse::clip_region(make_region("r", 1.0F, 10, 20, 30, 50), 100, 100);
  require(inside == cv::Rect(10, 20, 20, 30), "region inside the raster clips to itself");

  const auto overhang = grparse::clip_region(make_region("r", 1.0F, 80, 90, 200, 300), 100, 100);
  require(overhang == cv::Rect(80, 90, 20, 10), "region overhanging the raster clips to the edge");

  const auto negative = grparse::clip_region(make_region("r", 1.0F, -50, -50, 10, 10), 100, 100);
  require(negative == cv::Rect(0, 0, 10, 10), "negative coordinates clip to the origin");

  require(grparse::clip_region(make_region("r", 1.0F, 200, 200, 300, 300), 100, 100).empty(),
          "region entirely outside the raster is empty");
  require(grparse::clip_region(make_region("r", 1.0F, 40, 40, 40, 90), 100, 100).empty(),
          "zero-width region is empty");
  require(grparse::clip_region(make_region("r", 1.0F, 60, 60, 20, 90), 100, 100).empty(),
          "inverted region is empty");
}

void verify_crop_region() {
  cv::Mat raster(100, 100, CV_8UC3, cv::Scalar(1, 2, 3));
  const auto region = make_region("figure", 1.0F, 10, 20, 30, 50);

  cv::Mat crop = grparse::crop_region(raster, region);
  require(crop.cols == 20 && crop.rows == 30, "crop size matches the clipped region");

  // The crop must alias the raster (zero copy): writing through the view is
  // visible in the parent, and the view is marked non-continuous.
  crop.at<cv::Vec3b>(0, 0) = {9, 9, 9};
  require(raster.at<cv::Vec3b>(20, 10) == cv::Vec3b(9, 9, 9),
          "crop must be a zero-copy view of the raster");
  require(crop.datastart == raster.datastart, "crop shares the raster's buffer");

  require(grparse::crop_region(raster, make_region("r", 1.0F, 500, 500, 600, 600)).empty(),
          "crop of a region outside the raster is empty");
}

}  // namespace

int main() {
  try {
    verify_region_binding();
    verify_clip_region();
    verify_crop_region();
  } catch (const std::exception& error) {
    std::cerr << "region_geometry_test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "region_geometry_test passed\n";
  return EXIT_SUCCESS;
}
