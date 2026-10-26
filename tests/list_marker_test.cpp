// Proves the list marker rule: a list item's leading bullet or numbering
// moves from its text to its marker, the way the reference's
// ListItemMarkerProcessor does, so the Markdown export prints one bullet
// instead of "- - item" (the quickstart sample.pdf regression). Covers the
// splitter's pattern table, the assembled item (marker, text, orig, spans,
// offset row), and the Markdown, DocTags and canonical JSON exports of the
// sample's list.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.pb.h"
#include "grparse/document_assembly.h"
#include "grparse/document_render.h"
#include "grparse/list_marker.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

using grparse_test::require;

void expect_split(const std::string& orig, const std::string& marker, const std::string& text,
                  bool enumerated) {
  const auto split = grparse::split_list_marker(orig);
  require(split.has_value(), "'" + orig + "' opens with a marker");
  require(split->marker == marker, "'" + orig + "' marker is '" + marker + "', got '" +
                                       split->marker + "'");
  require(split->text == text, "'" + orig + "' text is '" + text + "', got '" + split->text + "'");
  require(split->enumerated == enumerated, "'" + orig + "' enumerated flag");
  require(split->prefix_code_points ==
              grparse::utf8_codepoint_count(orig) - grparse::utf8_codepoint_count(text),
          "'" + orig + "' prefix length counts code points");
}

void expect_no_split(const std::string& orig) {
  require(!grparse::split_list_marker(orig).has_value(), "'" + orig + "' keeps its text");
}

// Which words open another item of the list a lead marker opened: the same
// bullet glyph, any check box after a check box, or a numbering form of the
// lead's shape. Words of an item's own text are not.
void verify_sibling_markers() {
  using grparse::is_sibling_list_marker;
  require(is_sibling_list_marker("\xE2\x80\xA2", "\xE2\x80\xA2"), "a bullet after the same bullet");
  require(is_sibling_list_marker("-", "-"), "a dash after a dash");
  require(is_sibling_list_marker("\xE2\x98\x90", "\xE2\x98\x92"), "a crossed box after an empty one");
  require(is_sibling_list_marker("a)", "b)"), "b) after a)");
  require(is_sibling_list_marker("1.", "12."), "12. after 1.");
  require(is_sibling_list_marker("(1)", "(2)"), "(2) after (1)");
  require(is_sibling_list_marker("i.", "iv."), "iv. after i.");
  require(is_sibling_list_marker("a.", "c."), "c. after a., though c. also reads as roman");
  require(!is_sibling_list_marker("\xE2\x80\xA2", "-"), "a dash is not a bullet's sibling");
  require(!is_sibling_list_marker("1.", "1.5"), "a decimal is not a numbered item");
  require(!is_sibling_list_marker("1.", "J."), "an initial is not a numbered item");
  require(!is_sibling_list_marker("a)", "A)"), "case is part of the shape");
  require(!is_sibling_list_marker("Red", "Red"), "a word is no marker");
  require(!is_sibling_list_marker("- x", "- x"), "a marker is a whole word");
  require(!is_sibling_list_marker("", ""), "an empty word is no marker");
  require(is_sibling_list_marker("\xEF\xBC\x91.", "\xEF\xBC\x92."), "fullwidth 2. after 1.");
  require(is_sibling_list_marker("1)", "\xD9\xA3)"), "a digit is a digit in any script");
}

// The reference's pattern table, case by case, including its first-match
// order (compound numbering before simple, roman before single letters).
void verify_marker_patterns() {
  expect_split("- One page event", "-", "One page event", false);
  expect_split("* starred", "*", "starred", false);
  expect_split("+ plus", "+", "plus", false);
  expect_split("\xE2\x80\xA2 bullet", "\xE2\x80\xA2", "bullet", false);              // U+2022
  expect_split("\xC2\xB7 middle dot", "\xC2\xB7", "middle dot", false);              // U+00B7
  expect_split("\xE2\x97\x8F disc", "\xE2\x97\x8F", "disc", false);                  // U+25CF
  expect_split("\xE2\x9E\xA4 arrow", "\xE2\x9E\xA4", "arrow", false);                // U+27A4
  expect_split("\xE2\x9C\x93 done", "\xE2\x9C\x93", "done", false);                  // U+2713
  expect_split("\xE2\x80\xA2\xC2\xA0no-break", "\xE2\x80\xA2", "no-break", false);   // NBSP
  expect_split("-\ttab", "-", "tab", false);
  expect_split("- \nwrapped", "-", "\nwrapped", false);  // one separator, the rest is text
  expect_split("1. first", "1.", "first", true);
  expect_split("12) twelfth", "12)", "twelfth", true);
  expect_split("(3) third", "(3)", "third", true);
  expect_split("[4] fourth", "[4]", "fourth", true);
  expect_split("1.2.3 outline", "1.2.3", "outline", true);
  expect_split("1.1. outline", "1.1.", "outline", true);
  expect_split("9a. sub", "9a.", "sub", true);
  expect_split("3.a) sub", "3.a)", "sub", true);
  expect_split("(9a) sub", "(9a)", "sub", true);
  expect_split("iv. roman", "iv.", "roman", true);
  expect_split("IV. Roman", "IV.", "Roman", true);
  expect_split("b. letter", "b.", "letter", true);
  expect_split("B) Letter", "B)", "Letter", true);
  // Digits are any Unicode decimal digit; the prefix counts code points, so
  // spans after a multi-byte numbering form shift by its length in
  // characters, not bytes.
  expect_split("\xEF\xBC\x91. fullwidth", "\xEF\xBC\x91.", "fullwidth", true);  // U+FF11
  expect_split("\xD9\xA3) arabic-indic", "\xD9\xA3)", "arabic-indic", true);      // U+0663
  expect_split("(\xE0\xA5\xAA) devanagari", "(\xE0\xA5\xAA)", "devanagari", true);  // U+096A
  expect_split("\xEF\xBC\x91\xEF\xBC\x92.\xEF\xBC\x93 outline",
               "\xEF\xBC\x91\xEF\xBC\x92.\xEF\xBC\x93", "outline", true);
  expect_split("\xF0\x9D\x9F\x8F.a) math", "\xF0\x9D\x9F\x8F.a)", "math", true);  // U+1D7CF
  {
    const auto split = grparse::split_list_marker("\xEF\xBC\x91\xEF\xBC\x92. twelve");
    require(split.has_value() && split->prefix_code_points == 4,
            "fullwidth 12. plus its separator is four code points");
  }
  // Malformed UTF-8 is no marker and no separator.
  expect_no_split("\xC0\xAD x");          // overlong '-'
  expect_no_split("\xE0\x80\xAD x");     // overlong '-', three bytes
  expect_no_split("\xED\xA0\x80. x");    // U+D800, a surrogate
  expect_no_split("\xF4\x90\x80\x80. x");  // past U+10FFFF
  expect_no_split("-\xC1\xA0x");          // overlong space is no separator
  expect_no_split("\xC2\xB2. superscript");  // U+00B2 is no decimal digit (Nd)

  expect_no_split("");
  expect_no_split("-");
  expect_no_split("- ");        // a marker needs content after its separator
  expect_no_split("-5 degrees");
  expect_no_split("--- rule");
  expect_no_split("1 apple");
  expect_no_split("1.5x faster");
  expect_no_split("Plain prose");
  expect_no_split("ab. two letters");
  expect_no_split("\xE2\x80\x94 em dash");  // U+2014 is not in the reference's set
}

grparse::OcrLine line(std::string text, int top, std::string font = "Helvetica") {
  grparse::OcrLine member{std::move(text),
                          {{40, top}, {900, top}, {900, top + 20}, {40, top + 20}},
                          0.95F};
  member.font_name = std::move(font);
  member.font_size_pt = 11.0;
  return member;
}

// Page 1 of the quickstart's sample.pdf, as its text layer reads: a
// section header over three list items that carry a literal "- ".
grparse::OcrPage sample_page() {
  grparse::OcrPage page{1000, 1000,
                        {line("What the stream carries", 100, "Helvetica-Bold"),
                         line("- One page event per page, in page order, the moment each page "
                              "finishes",
                              140),
                         line("- Provenance boxes and append-only UTF offsets for every text item",
                              170),
                         line("- Table cell grids, picture classifications, and decoded barcodes",
                              200)}};
  page.source = grparse::OcrPage::Source::kDigitalPdf;
  page.regions = {
      {"section_header", 0.9F, 30, 95, 950, 125},
      {"list_item", 0.9F, 30, 135, 950, 165},
      {"list_item", 0.9F, 30, 165, 950, 195},
      {"list_item", 0.9F, 30, 195, 950, 225},
  };
  return page;
}

// The assembled item: marker set, text stripped, orig raw, the font run
// and the offset row measured on the stripped text, provenance on orig.
void verify_assembled_list_items() {
  grparse::AssemblyCursor cursor;
  ai::pipestream::parse::v1::PageData data;
  grparse::append_page_data(sample_page(), 1, &cursor, &data);
  require(data.texts_size() == 4, "a header and three list items");
  for (int index = 1; index < 4; ++index) {
    const auto& item = data.texts(index);
    require(item.has_list_item(), "a list_item region yields a ListItem");
    const auto& list_item = item.list_item();
    const auto& base = list_item.base();
    require(list_item.has_marker() && list_item.marker() == "-", "the bullet becomes the marker");
    require(!list_item.enumerated(), "a bullet is not enumerated");
    require(!base.text().starts_with("- ") && base.orig() == "- " + base.text(),
            "text loses the marker and orig keeps the raw line");
    const auto text_points = static_cast<int32_t>(grparse::utf8_codepoint_count(base.text()));
    require(base.spans_size() == 1 && base.spans(0).range().start() == 0 &&
                base.spans(0).range().end() == text_points,
            "the font run covers the stripped text");
    require(base.prov_size() == 1 && base.prov(0).charspan().start() == 0 &&
                base.prov(0).charspan().end() == text_points + 2,
            "provenance charspans keep indexing orig");
    const auto& offset = data.text_offsets(index);
    require(offset.utf_end() - offset.utf_start() == static_cast<uint64_t>(text_points),
            "the offset row measures the stripped text");
  }
  require(data.texts(2).list_item().base().text() ==
              "Provenance boxes and append-only UTF offsets for every text item",
          "the second item's text");
  require(data.texts(0).has_section_header() &&
              data.texts(0).section_header().base().text() == "What the stream carries",
          "a non-list item is left alone");

  // A numbered list item takes its numbering and the enumerated flag; a
  // list item with no marker keeps its text and no marker.
  grparse::AssemblyCursor numbered_cursor;
  grparse::OcrPage numbered{1000, 1000, {line("2. Second step", 100), line("No marker here", 140)}};
  numbered.regions = {{"list_item", 0.9F, 30, 95, 950, 125}, {"list_item", 0.9F, 30, 135, 950, 165}};
  ai::pipestream::parse::v1::PageData numbered_data;
  grparse::append_page_data(numbered, 1, &numbered_cursor, &numbered_data);
  require(numbered_data.texts_size() == 2, "two numbered-page items");
  const auto& second = numbered_data.texts(0).list_item();
  require(second.marker() == "2." && second.enumerated() && second.base().text() == "Second step",
          "numbering becomes an enumerated marker");
  const auto& unmarked = numbered_data.texts(1).list_item();
  require(!unmarked.has_marker() && unmarked.base().text() == "No marker here",
          "an item without a marker is unchanged");
}

// The sample's exports: one bullet per item in Markdown, the stripped text
// in DocTags and canonical JSON, the marker in canonical JSON.
void verify_sample_exports_agree() {
  grparse::AssemblyCursor cursor;
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_furniture()->set_self_ref("#/furniture");
  std::string plain_text;
  grparse::append_page_to_document(sample_page(), 1, &cursor, &document, &plain_text);
  grparse::group_list_items(&document);

  const std::string markdown = grparse::render_markdown(document);
  require(markdown.find("- - ") == std::string::npos, "no doubled bullet:\n" + markdown);
  for (const char* expected :
       {"- One page event per page, in page order, the moment each page finishes\n",
        "- Provenance boxes and append-only UTF offsets for every text item\n",
        "- Table cell grids, picture classifications, and decoded barcodes"}) {
    require(markdown.find(expected) != std::string::npos,
            std::string("Markdown carries '") + expected + "':\n" + markdown);
  }

  const std::string doctags = grparse::render_doctags(document);
  require(doctags.find("- One page event") == std::string::npos &&
              doctags.find("One page event per page") != std::string::npos,
          "DocTags carries the stripped text:\n" + doctags);

  const std::string json = grparse::render_canonical_json(document);
  require(json.find("\"text\": \"One page event per page") != std::string::npos ||
              json.find("\"text\":\"One page event per page") != std::string::npos,
          "canonical JSON text is stripped");
  require(json.find("\"orig\": \"- One page event per page") != std::string::npos ||
              json.find("\"orig\":\"- One page event per page") != std::string::npos,
          "canonical JSON orig keeps the raw line");
  require(json.find("\"marker\": \"-\"") != std::string::npos ||
              json.find("\"marker\":\"-\"") != std::string::npos,
          "canonical JSON carries the marker");
  require(plain_text.find("\n- ") == std::string::npos,
          "the plain text stream follows the stripped text");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("list-marker-test", "ok", {
      verify_marker_patterns,
      verify_sibling_markers,
      verify_assembled_list_items,
      verify_sample_exports_agree,
  });
}
