// Proves heading depth inference: numbering patterns, all-caps section
// words, the first-page title block (merged into one item on a Document),
// unnumbered headings taking the nearest numbered cluster's depth, the
// legacy height clustering when nothing is numbered, the producer gate
// (a structural collector's level wins, a text-layer collector's is
// re-derived), and idempotence.

#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <google/protobuf/util/message_differencer.h>

#include "grparse/heading_hierarchy.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

using grparse_test::require;

grparse::HeaderHeight header(const std::string& ref, const std::string& text, double height,
                             int page, double top) {
  grparse::HeaderHeight entry;
  entry.self_ref = ref;
  entry.text = text;
  entry.height = height;
  entry.page = page;
  entry.top = top;
  entry.bottom = top + height;
  return entry;
}

void verify_numbering_depths() {
  using grparse::heading_numbering_depth;
  require(heading_numbering_depth("1 INTRODUCTION") == 1, "1");
  require(heading_numbering_depth("2. Background") == 1, "2.");
  require(heading_numbering_depth("3.1 TRAINING THE MODEL") == 2, "3.1");
  require(heading_numbering_depth("4.2.3 Details") == 3, "4.2.3");
  require(heading_numbering_depth("10) Results") == 1, "10)");
  require(heading_numbering_depth("A. Proofs") == 1, "A.");
  require(heading_numbering_depth("B) More proofs") == 1, "B)");
  require(heading_numbering_depth("A.1 Lemma") == 2, "A.1");
  require(heading_numbering_depth("IV. Evaluation") == 1, "IV.");
  require(heading_numbering_depth("I INTRODUCTION") == 1, "I followed by a capital word");
  require(heading_numbering_depth("Appendix A") == 1, "Appendix A");
  require(heading_numbering_depth("APPENDIX B.2 Extra tables") == 2, "APPENDIX B.2");
  require(heading_numbering_depth("Appendix: proofs") == 1, "Appendix with words");
  require(!heading_numbering_depth("ABSTRACT").has_value(), "a word is not numbering");
  require(!heading_numbering_depth("12").has_value(), "a bare number is a page number");
  require(!heading_numbering_depth("2023 was a year").has_value(), "a year is not numbering");
  require(!heading_numbering_depth("A short heading").has_value(), "the article A is a word");
  require(!heading_numbering_depth("I think so").has_value(), "the pronoun I is a word");
  require(!heading_numbering_depth("F. Scott Fitzgerald (1925). Public domain text from the "
                                   "Project Gutenberg transcription.").has_value(),
          "an initial opening a long line is not an enumerator");
  require(heading_numbering_depth("F. Scott Fitzgerald") == 1,
          "a short line opening with a letter and a period still counts as enumerated");
  require(!heading_numbering_depth("").has_value(), "empty");
}

void verify_section_words() {
  using grparse::is_section_word_heading;
  require(is_section_word_heading("ABSTRACT"), "ABSTRACT");
  require(is_section_word_heading("RELATED WORK"), "RELATED WORK");
  require(is_section_word_heading(" ACKNOWLEDGMENTS "), "padded");
  require(!is_section_word_heading("1 INTRODUCTION"), "numbered");
  require(!is_section_word_heading("Anonymous authors"), "mixed case");
  require(!is_section_word_heading("A B"), "too few letters");
  require(!is_section_word_heading("ONE TWO THREE FOUR"), "too many words");
}

// The paper's headings: two title lines, an authors line, ABSTRACT, and
// numbered sections all 12 high, one of them with a subsection.
std::vector<grparse::HeaderHeight> paper_headings() {
  return {
      header("#/texts/56", "CODE DIFFUSION MODELS ARE CONTINUOUS HUMAN", 17.2, 1, 76),
      header("#/texts/57", "NOISE OPERATORS", 13.8, 1, 100),
      header("#/texts/58", "Anonymous authors", 10.0, 1, 135),
      header("#/texts/60", "ABSTRACT", 12.0, 1, 185),
      header("#/texts/62", "1 INTRODUCTION", 12.0, 1, 383),
      header("#/texts/134", "2 BACKGROUND", 12.0, 2, 477),
      header("#/texts/206", "3.1 TRAINING THE DIFFUSION MODEL", 11.0, 3, 633),
      header("#/texts/588", "REFERENCES", 12.0, 9, 100),
  };
}

void verify_paper_levels() {
  const auto title = grparse::title_lines(paper_headings());
  require(title == std::vector<std::string>{"#/texts/56", "#/texts/57"},
          "the two adjacent lines opening page 1 are the title");
  const auto levels = grparse::infer_heading_levels(paper_headings());
  require(levels.at("#/texts/56") == 1 && levels.at("#/texts/57") == 1, "title lines report level 1");
  require(levels.at("#/texts/60") == 1, "ABSTRACT is a peer of the numbered sections");
  require(levels.at("#/texts/62") == 1 && levels.at("#/texts/134") == 1, "1 and 2 are level 1");
  require(levels.at("#/texts/206") == 2, "3.1 is level 2");
  require(levels.at("#/texts/588") == 1, "REFERENCES is level 1");
  require(levels.at("#/texts/58") == 2,
          "the unnumbered authors line takes the nearest cluster, the subsections");
}

// Without a title the numbered depth is the level; an unnumbered heading
// takes the nearest cluster.
void verify_numbering_without_title() {
  const std::vector<grparse::HeaderHeight> headings = {
      header("#/texts/1", "1 Scope", 20, 1, 100),
      header("#/texts/2", "1.1 Purpose", 14, 1, 200),
      header("#/texts/3", "Overview", 19, 1, 300),
      header("#/texts/4", "Notes", 14.5, 1, 400),
      header("#/texts/5", "2 Terms", 20, 2, 100),
  };
  const auto levels = grparse::infer_heading_levels(headings);
  require(levels.at("#/texts/1") == 1 && levels.at("#/texts/5") == 1, "numbered depth 1");
  require(levels.at("#/texts/2") == 2, "numbered depth 2");
  require(levels.at("#/texts/3") == 1, "unnumbered near the depth-1 cluster");
  require(levels.at("#/texts/4") == 2, "unnumbered near the depth-2 cluster");
}

// Nothing numbered: the legacy clustering, tallest first, 85% rule, and a
// first heading that is no taller than a later one is no title.
void verify_legacy_clustering() {
  const std::vector<grparse::HeaderHeight> headings = {
      header("#/texts/0", "Chapter", 40, 1, 100),
      header("#/texts/1", "Subsection", 20, 1, 300),
      header("#/texts/2", "Another chapter", 40, 1, 600),
      header("#/texts/3", "Unmeasured", 0, 1, 700),
  };
  require(grparse::title_lines(headings).empty(), "equal heights make no title");
  require(grparse::title_lines({header("#/texts/0", "Only", 40, 1, 100)}).empty(),
          "a lone heading is no title");
  const auto levels = grparse::infer_heading_levels(headings);
  require(levels.at("#/texts/0") == 1 && levels.at("#/texts/2") == 1, "tallest cluster is 1");
  require(levels.at("#/texts/1") == 2, "smaller is 2");
  require(levels.at("#/texts/3") == 1, "no height, level 1");
  require(grparse::infer_heading_levels({}).empty(), "no headings, no levels");
}

// Font sizes beat heights when every heading declares one.
void verify_font_sizes_win_when_complete() {
  std::vector<grparse::HeaderHeight> headings = {
      header("#/texts/0", "Big by height", 40, 1, 100),
      header("#/texts/1", "Small by height", 20, 1, 300),
  };
  headings[0].font_size = 10;
  headings[1].font_size = 18;
  const auto levels = grparse::infer_heading_levels(headings);
  require(levels.at("#/texts/1") == 1 && levels.at("#/texts/0") == 2, "font sizes decide");
}

// --- Document side -------------------------------------------------------

docv1::Document base_document() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  auto& page = (*document.mutable_pages())[1];
  page.set_page_no(1);
  page.mutable_size()->set_width(612);
  page.mutable_size()->set_height(792);
  return document;
}

std::string add_header(docv1::Document* document, const std::string& text, int level, double top,
                       double height, const char* collector, double left = 108,
                       double right = 504) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* item = document->add_texts()->mutable_section_header();
  item->set_level(level);
  auto* base = item->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_label(docv1::DOC_ITEM_LABEL_SECTION_HEADER);
  base->set_text(text);
  auto* prov = base->add_prov();
  prov->set_page_no(1);
  auto* box = prov->mutable_bbox();
  box->set_l(left);
  box->set_r(right);
  box->set_t(top);
  box->set_b(top + height);
  box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  auto* span = base->add_spans();
  span->mutable_range()->set_start(0);
  span->mutable_range()->set_end(static_cast<int32_t>(text.size()));
  span->set_font_family("Serif");
  if (collector != nullptr) base->add_source()->mutable_collector()->set_collector(collector);
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

std::string add_prose(docv1::Document* document, const std::string& text, double top) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* base = document->add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->set_text(text);
  auto* prov = base->add_prov();
  prov->set_page_no(1);
  auto* box = prov->mutable_bbox();
  box->set_l(108);
  box->set_r(504);
  box->set_t(top);
  box->set_b(top + 10);
  box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  base->add_source()->mutable_collector()->set_collector("pdf");
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

// A pdf-collector document with the paper's shape: producer levels are
// per-page guesses and get re-derived, the title lines merge into one item
// with provenance, spans and body references carried and renumbered.
void verify_document_title_merge_and_levels() {
  docv1::Document document = base_document();
  const std::string line_1 = add_header(&document, "CODE DIFFUSION MODELS", 1, 76, 17.2, "pdf");
  const std::string line_2 = add_header(&document, "NOISE OPERATORS", 2, 100, 13.8, "pdf", 108, 243);
  add_header(&document, "Anonymous authors", 4, 135, 10.0, "pdf", 114, 200);
  add_prose(&document, "Paper under double-blind review", 146);
  add_header(&document, "ABSTRACT", 3, 185, 12.0, "pdf", 278, 334);
  const std::string prose = add_prose(&document, "Diffusion for code generates code", 210);
  add_header(&document, "1 INTRODUCTION", 3, 383, 12.0, "pdf", 108, 206);
  add_header(&document, "3.1 TRAINING", 1, 633, 11.0, "pdf", 108, 300);
  // A reference into the second title line from elsewhere follows it.
  document.mutable_texts(5)->mutable_text()->mutable_base()->add_children()->set_ref(line_2);

  const grparse::HeadingReport report = grparse::infer_heading_hierarchy(&document);
  require(report.titles_merged == 1 && report.titles_promoted == 1,
          "the second title line folds into the first, which becomes the title");
  require(document.texts_size() == 7, "the arena shrank by one");
  require(document.texts(0).item_case() == docv1::BaseTextItem::kTitle, "the title is a TitleItem");
  const auto& title = document.texts(0).title().base();
  require(title.text() == "CODE DIFFUSION MODELS NOISE OPERATORS", "title text joins");
  require(title.label() == docv1::DOC_ITEM_LABEL_TITLE && title.self_ref() == "#/texts/0",
          "the title keeps its reference and takes the title label");
  require(title.prov_size() == 2, "both lines' boxes stay");
  require(title.spans_size() == 2 && title.spans(1).range().start() == 22,
          "the second line's span shifts past the first line and the space");
  require(document.body().children_size() == 7 && document.body().children(1).ref() == "#/texts/1",
          "body references renumber");
  require(document.texts(4).text().base().children(0).ref() == "#/texts/0",
          "a reference into the folded line follows it to the title");
  require(document.texts(1).section_header().level() == 2, "the authors line joins the subsections");
  require(document.texts(3).section_header().level() == 1, "ABSTRACT is level 1");
  require(document.texts(5).section_header().level() == 1, "1 INTRODUCTION is level 1");
  require(document.texts(6).section_header().level() == 2, "3.1 is level 2");
  require(report.levels_assigned == 4, "four levels changed: authors, ABSTRACT, 1, 3.1");

  const docv1::Document once = document;
  const grparse::HeadingReport again = grparse::infer_heading_hierarchy(&document);
  require(again.titles_merged == 0 && again.titles_promoted == 0 && again.levels_assigned == 0 &&
              google::protobuf::util::MessageDifferencer::Equals(once, document),
          "a second run changes nothing");
}

// A geometry collector's headings that read as prose go back to being
// text; a structural producer's stay whatever they say.
void verify_prose_headings_demoted() {
  using grparse::heading_reads_as_prose;
  require(heading_reads_as_prose("\xE2\x80\x9CI\xE2\x80\x99m p-paralysed with happiness.\xE2\x80\x9D"),
          "a quoted line");
  require(heading_reads_as_prose("I told him."), "a sentence");
  require(heading_reads_as_prose("Her host looked at her incredulously."), "another sentence");
  require(!heading_reads_as_prose("1. West Egg"), "a numbered heading");
  require(!heading_reads_as_prose("ABSTRACT"), "a section word");
  require(!heading_reads_as_prose("2.1 The Garage on the Edge of the Waste Land"), "a long heading");
  require(!heading_reads_as_prose("Results: an overview"), "a colon is not sentence punctuation");

  docv1::Document document = base_document();
  add_header(&document, "The Great Gatsby Reset", 1, 60, 43, "pdf");
  add_header(&document, "1. West Egg", 1, 120, 13, "pdf");
  const std::string quoted =
      add_header(&document, "\xE2\x80\x9CI\xE2\x80\x99" "d like to.\xE2\x80\x9D", 2, 300, 10, "pdf");
  add_header(&document, "This annoyed me.", 2, 400, 10, "libreoffice");
  const grparse::HeadingReport report = grparse::infer_heading_hierarchy(&document);
  require(report.headings_demoted == 1, "one heading is demoted");
  require(document.texts(2).item_case() == docv1::BaseTextItem::kText &&
              document.texts(2).text().base().label() == docv1::DOC_ITEM_LABEL_TEXT &&
              document.texts(2).text().base().self_ref() == quoted &&
              document.texts(2).text().base().prov_size() == 1,
          "the quoted line is a text item again with its reference and box");
  require(document.texts(3).item_case() == docv1::BaseTextItem::kSectionHeader &&
              document.texts(3).section_header().level() == 2,
          "a structural producer's heading is not touched");
  require(document.texts(0).item_case() == docv1::BaseTextItem::kTitle &&
              document.texts(1).section_header().level() == 1,
          "the title is promoted and the numbered heading is level 1");
}

// A structural producer's level is kept; a level-less header beside it is
// still inferred; title lines are not merged when they are not consecutive
// body children.
void verify_producer_levels_win() {
  docv1::Document document = base_document();
  add_header(&document, "Sample Document", 1, 60, 30, "libreoffice");
  add_prose(&document, "between the lines", 95);
  add_header(&document, "Second line", 2, 110, 30, "libreoffice");
  add_header(&document, "Headings", 4, 200, 20, "libreoffice");
  const std::string levelless = add_header(&document, "2 Lists", 0, 300, 20, nullptr);
  const grparse::HeadingReport report = grparse::infer_heading_hierarchy(&document);
  require(report.titles_merged == 0, "nothing merges across a paragraph");
  require(document.texts(0).section_header().level() == 1 &&
              document.texts(2).section_header().level() == 2 &&
              document.texts(3).section_header().level() == 4,
          "a structural producer's levels stay");
  require(document.texts(4).section_header().level() == 1 && report.levels_assigned == 1,
          "the level-less header takes its numbered depth");

  grparse::HeadingOptions no_merge;
  docv1::Document adjacent = base_document();
  add_header(&adjacent, "One", 0, 60, 30, nullptr);
  add_header(&adjacent, "Two", 0, 95, 30, nullptr);
  add_header(&adjacent, "1 Small", 0, 300, 12, nullptr);
  no_merge.merge_title_lines = false;
  const grparse::HeadingReport unmerged = grparse::infer_heading_hierarchy(&adjacent, no_merge);
  require(adjacent.texts_size() == 3 && unmerged.titles_promoted == 0 &&
              adjacent.texts(0).section_header().level() == 1 &&
              adjacent.texts(1).section_header().level() == 1 &&
              adjacent.texts(2).section_header().level() == 1,
          "with merging off a two-line title stays two level-1 headers and nothing is promoted");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("heading-hierarchy-test", "ok", {
      verify_numbering_depths,
      verify_section_words,
      verify_paper_levels,
      verify_numbering_without_title,
      verify_legacy_clustering,
      verify_font_sizes_win_when_complete,
      verify_document_title_merge_and_levels,
      verify_prose_headings_demoted,
      verify_producer_levels_win,
  });
}
