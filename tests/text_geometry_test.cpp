#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "grparse/ocr_types.h"
#include "grparse/text_geometry.h"
#include "grparse/document_assembly.h"

#include "ai/pipestream/parse/v1/parse_stream.pb.h"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

grparse::OcrLine make_line(std::string text, int left, int top, int right, int bottom,
                           grparse::TextOrigin origin, float confidence = 0.8F) {
  return grparse::OcrLine{std::move(text),
                          {{left, top}, {right, top}, {right, bottom}, {left, bottom}},
                          confidence,
                          origin};
}

void verify_iou_and_overlap() {
  const auto a = grparse::bounding_box(make_line("a", 0, 0, 10, 10, grparse::TextOrigin::kOcr));
  const auto b = grparse::bounding_box(make_line("b", 5, 5, 15, 15, grparse::TextOrigin::kOcr));
  const auto c = grparse::bounding_box(make_line("c", 50, 50, 60, 60, grparse::TextOrigin::kOcr));
  require(grparse::intersection_over_union(a, b) > 0.0F, "overlapping boxes have iou");
  require(grparse::boxes_overlap_significantly(a, b), "center/iou overlap");
  require(!grparse::boxes_overlap_significantly(a, c), "distant boxes");

  const auto identical = grparse::intersection_over_union(a, a);
  require(identical > 0.99F && identical <= 1.0F, "self overlap is total");
  require(grparse::intersection_over_union(a, c) == 0.0F, "disjoint boxes have no overlap");
}

// OCR polygons are model output: a degenerate box must not wrap an int multiply
// and turn a non-overlap into a spurious duplicate (or the reverse).
void verify_extreme_coordinates_do_not_overflow() {
  constexpr int kHuge = 1 << 30;
  const grparse::AxisAlignedBox wide{0, 0, kHuge, kHuge};
  const grparse::AxisAlignedBox inner{10, 10, 20, 20};
  require(wide.area() > 0, "large box area must stay positive");

  const float iou = grparse::intersection_over_union(wide, inner);
  require(iou >= 0.0F && iou <= 1.0F, "iou must stay a ratio for large boxes");
  require(iou < 0.01F, "a tiny box barely overlaps a huge one");
  // The tiny box's centre is inside the huge one, so containment still wins.
  require(grparse::boxes_overlap_significantly(wide, inner), "containment detects the overlap");

  const grparse::AxisAlignedBox negative{-kHuge, -kHuge, -1, -1};
  require(grparse::intersection_over_union(negative, inner) == 0.0F,
          "boxes on opposite sides of the origin do not overlap");
  require(!grparse::boxes_overlap_significantly(negative, inner),
          "negative coordinates must not alias into an overlap");

  const grparse::AxisAlignedBox empty{5, 5, 5, 5};
  require(empty.area() == 0, "degenerate box has no area");
  require(grparse::intersection_over_union(empty, inner) == 0.0F, "degenerate box has no overlap");
}

void verify_merge_dedupes_and_sorts() {
  grparse::OcrPage digital{200, 200, {make_line("header", 0, 0, 40, 10, grparse::TextOrigin::kDigitalPdf),
                                      make_line("dup", 10, 40, 50, 50, grparse::TextOrigin::kDigitalPdf)}};
  digital.source = grparse::OcrPage::Source::kDigitalPdf;

  grparse::OcrPage ocr{
      200, 200,
      {make_line("dup-ocr", 12, 42, 48, 48, grparse::TextOrigin::kOcr),  // overlaps digital dup
       make_line("footer", 0, 180, 80, 190, grparse::TextOrigin::kOcr),
       make_line("mid", 0, 80, 60, 90, grparse::TextOrigin::kOcr)}};

  const grparse::OcrPage merged = grparse::merge_digital_and_ocr(digital, ocr);
  require(merged.source == grparse::OcrPage::Source::kMerged, "merged source tag");
  require(merged.lines.size() == 4, "dup OCR dropped, three uniques + mid");
  require(merged.lines[0].text == "header", "reading order top");
  require(merged.lines[1].text == "dup", "digital wins on overlap");
  require(merged.lines[2].text == "mid", "ocr mid");
  require(merged.lines[3].text == "footer", "ocr footer");

  grparse::AssemblyCursor cursor;
  ai::pipestream::parse::v1::PageData page;
  grparse::append_page_data(merged, 1, &cursor, &page);
  require(page.text_offsets_size() == 4, "offsets for merged lines");
  require(page.text_offsets(0).source() == ai::pipestream::parse::v1::TEXT_SOURCE_DIGITAL_PDF,
          "per-line digital source");
  require(page.text_offsets(2).source() == ai::pipestream::parse::v1::TEXT_SOURCE_OCR,
          "per-line ocr source");
}

}  // namespace

int main() {
  try {
    verify_iou_and_overlap();
    verify_extreme_coordinates_do_not_overflow();
    verify_merge_dedupes_and_sorts();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "text-geometry-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
