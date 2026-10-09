// Exercises the single region-binding rule (highest-confidence region
// containing the line's box center) and the raster clipping/cropping helpers
// that table structure, figure classification, and barcode decode all rely on.
#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "grparse/ocr_types.h"
#include "grparse/region_geometry.h"
#include "support/check.h"

namespace {

using grparse_test::require;

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
                  make_region("picture", 0.5F, 50, 50, 150, 150),
                  make_region("text", 0.7F, 400, 400, 500, 500)};

  // Center (75, 75) lies inside both the table and the figure; confidence wins.
  const auto* both = grparse::region_for_line(page, make_line(60, 60, 90, 90));
  require(both != nullptr && both->label == "table", "highest-confidence containing region wins");

  // Center (125, 125) lies only inside the figure.
  const auto* figure_only = grparse::region_for_line(page, make_line(110, 110, 140, 140));
  require(figure_only != nullptr && figure_only->label == "picture",
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
  const auto region = make_region("picture", 1.0F, 10, 20, 30, 50);

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

std::vector<std::string> labels_of(const std::vector<grparse::LayoutRegion>& regions) {
  std::vector<std::string> labels;
  for (const auto& region : regions) labels.push_back(region.label);
  return labels;
}

// A scanned ledger the detector boxed as one page-sized picture (court
// filing p848): the picture is the page, so it goes when text lies inside
// it and the page's own header and text regions take the lines. A photo
// scan with no text keeps the picture as its only content, and a picture
// under the 90% share stays whatever it holds.
void verify_full_page_picture_with_text_is_dropped() {
  const std::vector<grparse::OcrLine> lines = {make_line(810, 210, 1310, 255),
                                               make_line(120, 400, 300, 430)};
  std::vector<grparse::LayoutRegion> ledger = {
      make_region("section_header", 0.60F, 805, 208, 1317, 250),
      make_region("picture", 0.57F, 0, 3, 2200, 1701),
      make_region("text", 0.51F, 94, 260, 1896, 1499)};
  grparse::resolve_region_overlaps(ledger, lines, 2200, 1701);
  require(labels_of(ledger) == std::vector<std::string>{"section_header", "text"},
          "a page-sized picture over text is dropped");

  std::vector<grparse::LayoutRegion> photo = {make_region("picture", 0.9F, 0, 0, 2200, 1701)};
  grparse::resolve_region_overlaps(photo, {}, 2200, 1701);
  require(photo.size() == 1, "a page-sized picture with no text inside stays");

  std::vector<grparse::LayoutRegion> figure = {make_region("picture", 0.9F, 0, 0, 2000, 1600)};
  grparse::resolve_region_overlaps(figure, lines, 2200, 1701);
  require(figure.size() == 1, "a picture under 90% of the page stays");
}

// The same table detected twice, once nested in the other (court filing
// p862): the group keeps the larger box at a near score, and drops it for a
// smaller one only when that one scores more than 0.2 higher. Tables that
// merely touch stay two.
void verify_overlapping_tables_keep_one() {
  std::vector<grparse::LayoutRegion> nested = {make_region("table", 0.564F, 313, 215, 1880, 1130),
                                               make_region("table", 0.554F, 76, 221, 1909, 1543)};
  grparse::resolve_region_overlaps(nested, {}, 2200, 1701);
  require(nested.size() == 1 && nested[0].left == 76 && nested[0].bottom == 1543,
          "a table nested in another one keeps the larger box");

  std::vector<grparse::LayoutRegion> confident = {make_region("table", 0.95F, 313, 215, 1880, 1130),
                                                  make_region("table", 0.60F, 76, 221, 1909, 1543)};
  grparse::resolve_region_overlaps(confident, {}, 2200, 1701);
  require(confident.size() == 1 && confident[0].left == 313,
          "a much surer inner table wins over the larger box");

  std::vector<grparse::LayoutRegion> apart = {make_region("table", 0.8F, 0, 0, 1000, 500),
                                              make_region("table", 0.7F, 0, 450, 1000, 1000)};
  grparse::resolve_region_overlaps(apart, {}, 2200, 1701);
  require(apart.size() == 2, "tables that only touch stay two");
}

// A text region the detector drew around one amount inside a table, at a
// higher score than the table (court filing p772): it is part of the table,
// so it goes and the line binds to the table. Regions mostly outside the
// table, and the furniture beside it, stay; so do floats inside it.
void verify_regions_nested_in_a_table_join_it() {
  std::vector<grparse::LayoutRegion> regions = {
      make_region("page_header", 0.556F, 2047, 457, 2079, 1278),
      make_region("text", 0.551F, 1817, 1322, 1896, 1342),
      make_region("table", 0.510F, 93, 200, 1924, 1527),
      make_region("text", 0.6F, 1800, 100, 2000, 300),
      make_region("picture", 0.7F, 200, 300, 400, 500)};
  grparse::resolve_region_overlaps(regions, {}, 2200, 1701);
  require(labels_of(regions) ==
              std::vector<std::string>{"page_header", "table", "text", "picture"},
          "only the region nested in the table goes");

  grparse::OcrPage page;
  page.width = 2200;
  page.height = 1701;
  page.regions = regions;
  const auto* bound = grparse::region_for_line(page, make_line(1812, 1319, 1900, 1346));
  require(bound != nullptr && bound->label == "table", "the amount binds to the table");
}

}  // namespace

int main() {
  return grparse_test::run_test_main({.on_failure = "region_geometry_test failed", .on_success = "region_geometry_test passed"}, {
      verify_region_binding,
      verify_clip_region,
      verify_crop_region,
      verify_full_page_picture_with_text_is_dropped,
      verify_overlapping_tables_keep_one,
      verify_regions_nested_in_a_table_join_it,
  });
}
