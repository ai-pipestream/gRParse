#include <cstdio>
#include <cstdlib>
#include <print>
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

// A page whose detected lines are tall and narrow was rasterized a
// quarter turn from upright; a normal page, or a page with too few lines
// to judge, is not.
void verify_rotation_vote() {
  grparse::OcrPage upright{1000, 1000, {}};
  for (int line = 0; line < 5; ++line) {
    upright.lines.push_back(make_line("w", 50, 100 + 30 * line, 600, 120 + 30 * line,
                                      grparse::TextOrigin::kOcr));
  }
  const auto flat = grparse::page_rotation_vote(upright);
  require(flat.horizontal_lines == 5 && flat.vertical_lines == 0 && !flat.quarter_turn,
          "wide lines vote upright");

  grparse::OcrPage turned{1000, 1000, {}};
  for (int line = 0; line < 5; ++line) {
    turned.lines.push_back(make_line("w", 100 + 30 * line, 50, 120 + 30 * line, 600,
                                     grparse::TextOrigin::kOcr));
  }
  turned.lines.push_back(make_line("x", 10, 10, 200, 30, grparse::TextOrigin::kOcr));
  const auto tall = grparse::page_rotation_vote(turned);
  require(tall.vertical_lines == 5 && tall.horizontal_lines == 1 && tall.quarter_turn,
          "tall lines outvote one wide line");

  grparse::OcrPage sparse{1000, 1000, {}};
  sparse.lines.push_back(make_line("w", 100, 50, 120, 600, grparse::TextOrigin::kOcr));
  sparse.lines.push_back(make_line("w", 140, 50, 160, 600, grparse::TextOrigin::kOcr));
  require(!grparse::page_rotation_vote(sparse).quarter_turn, "two lines are too few to judge");

  grparse::OcrPage squares{1000, 1000, {}};
  squares.lines.push_back(make_line("w", 100, 100, 120, 125, grparse::TextOrigin::kOcr));
  squares.lines.push_back(grparse::OcrLine{"", {}, std::nullopt});
  const auto square = grparse::page_rotation_vote(squares);
  require(square.vertical_lines == 0 && square.horizontal_lines == 0,
          "near-square and empty boxes cast no vote");
}

// The read-quality summary, the candidate turns it and the vote produce,
// and the ranking between reads of one page.
void verify_read_quality_candidates_and_ranking() {
  grparse::OcrPage upright{1000, 1000, {}};
  for (int line = 0; line < 4; ++line) {
    upright.lines.push_back(make_line("w", 50, 100 + 30 * line, 600, 120 + 30 * line,
                                      grparse::TextOrigin::kOcr, 0.9F));
  }
  const auto clean = grparse::page_read_quality(upright);
  require(clean.lines == 4 && clean.scored_lines == 4 && !clean.upside_down &&
              clean.mean_confidence > 0.89F && clean.mean_confidence < 0.91F,
          "a clean read: every line scored, none flipped");
  require(grparse::rotation_candidates(grparse::page_rotation_vote(upright), clean).empty(),
          "a clean upright read needs no re-read");

  grparse::OcrPage flipped = upright;
  for (auto& line : flipped.lines) line.flipped = true;
  const auto upside_down = grparse::page_read_quality(flipped);
  require(upside_down.flipped_lines == 4 && upside_down.upside_down,
          "every line flipped is an upside-down page");
  require(grparse::rotation_candidates(grparse::page_rotation_vote(flipped), upside_down) ==
              std::vector<int>{180},
          "an upside-down read tries the half turn only");

  grparse::OcrPage turned{1000, 1000, {}};
  for (int line = 0; line < 4; ++line) {
    turned.lines.push_back(make_line("w", 100 + 30 * line, 50, 120 + 30 * line, 600,
                                     grparse::TextOrigin::kOcr, 0.9F));
  }
  require(grparse::rotation_candidates(grparse::page_rotation_vote(turned),
                                       grparse::page_read_quality(turned)) ==
              std::vector<int>{90, 270},
          "a quarter-turn vote tries both quarter turns");

  grparse::OcrPage poor = upright;
  for (auto& line : poor.lines) line.confidence = 0.3F;
  require(grparse::rotation_candidates(grparse::page_rotation_vote(poor),
                                       grparse::page_read_quality(poor)) ==
              std::vector<int>{90, 180, 270},
          "a poor upright read tries every turn");
  require(grparse::rotation_candidates(grparse::page_rotation_vote(poor),
                                       grparse::page_read_quality(poor), 0.2F)
              .empty(),
          "the floor decides what poor means");
  grparse::OcrPage sparse_poor{1000, 1000, {poor.lines[0], poor.lines[1]}};
  require(grparse::rotation_candidates(grparse::page_rotation_vote(sparse_poor),
                                       grparse::page_read_quality(sparse_poor))
              .empty(),
          "two poor lines are too few to justify three re-reads");

  grparse::OcrPage unscored = upright;
  for (auto& line : unscored.lines) line.confidence = std::nullopt;
  const auto silent = grparse::page_read_quality(unscored);
  require(silent.scored_lines == 0 && silent.mean_confidence == 0.0F, "no scores, no mean");
  require(grparse::rotation_candidates(grparse::page_rotation_vote(unscored), silent).empty(),
          "a read without confidences is never called poor");

  require(grparse::score_read(upright) > grparse::score_read(flipped),
          "an upright read outranks an upside-down one");
  require(grparse::score_read(upright) > grparse::score_read(turned),
          "an upright read outranks a quarter-turned one");
  require(grparse::score_read(poor) > grparse::score_read(flipped),
          "upright beats flipped even at lower confidence");
  require(grparse::score_read(upright) > grparse::score_read(poor),
          "among upright reads the confident one wins");
  require(grparse::score_read(turned) > grparse::score_read(grparse::OcrPage{1000, 1000, {}}),
          "any text beats no text");

  // Two tall lines and one wide one cast no quarter-turn vote (too few),
  // yet the read is not upright evidence: a turned read of a sparse page
  // must not outrank the page as fed on confidence alone.
  grparse::OcrPage sparse_tall{1000, 1000, {}};
  sparse_tall.lines.push_back(make_line("t", 100, 50, 120, 600, grparse::TextOrigin::kOcr, 0.99F));
  sparse_tall.lines.push_back(make_line("t", 140, 50, 160, 600, grparse::TextOrigin::kOcr, 0.99F));
  sparse_tall.lines.push_back(make_line("w", 50, 700, 600, 720, grparse::TextOrigin::kOcr, 0.99F));
  require(!grparse::page_rotation_vote(sparse_tall).quarter_turn, "two tall lines are no vote");
  require(!grparse::score_read(sparse_tall).upright, "tall-majority reads are not upright");
  require(grparse::score_read(poor) > grparse::score_read(sparse_tall),
          "a poor upright read still outranks a confident tall-majority one");
}

int main() {
  try {
    verify_iou_and_overlap();
    verify_extreme_coordinates_do_not_overflow();
    verify_merge_dedupes_and_sorts();
    verify_rotation_vote();
    verify_read_quality_candidates_and_ranking();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "text-geometry-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
