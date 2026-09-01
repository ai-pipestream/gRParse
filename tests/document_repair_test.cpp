// Proves the post-merge repair pass on hand-built Documents: running
// headers and page numbers demoted to furniture while section headers and
// single-page documents are left alone, line-break hyphenation rejoined
// with known compounds kept and soft hyphens dropped, paragraphs a page
// break split merged with provenance appended and every reference
// renumbered, the cases that must not merge, and idempotence of the whole
// pass.

#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <google/protobuf/util/message_differencer.h>

#include "grparse/document_repair.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

docv1::Document base_document() {
  docv1::Document document;
  document.set_name("repair.pdf");
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  return document;
}

void add_pages(docv1::Document* document, int count, double height = 1000.0) {
  for (int page = 1; page <= count; ++page) {
    auto& item = (*document->mutable_pages())[page];
    item.set_page_no(page);
    item.mutable_size()->set_width(800.0);
    item.mutable_size()->set_height(height);
  }
}

void place(docv1::TextItemBase* base, int page, double top, double bottom) {
  auto* prov = base->add_prov();
  prov->set_page_no(page);
  auto* box = prov->mutable_bbox();
  box->set_l(60.0);
  box->set_t(top);
  box->set_r(740.0);
  box->set_b(bottom);
  box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
}

// A body-level TextItem with the given label, placed on a page.
std::string add_prose(docv1::Document* document, const std::string& text, int page, double top,
                      double bottom, docv1::DocItemLabel label = docv1::DOC_ITEM_LABEL_TEXT) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* base = document->add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_label(label);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  place(base, page, top, bottom);
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

std::string add_section_header(docv1::Document* document, const std::string& text, int page,
                               double top, double bottom) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* item = document->add_texts()->mutable_section_header();
  item->set_level(1);
  auto* base = item->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_label(docv1::DOC_ITEM_LABEL_SECTION_HEADER);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  place(base, page, top, bottom);
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

std::string add_list_item(docv1::Document* document, const std::string& text, int page,
                          double top, double bottom) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* base = document->add_texts()->mutable_list_item()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_label(docv1::DOC_ITEM_LABEL_LIST_ITEM);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  place(base, page, top, bottom);
  document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

const docv1::TextItemBase& base_at(const docv1::Document& document, const std::string& ref) {
  const int index = std::stoi(ref.substr(std::string("#/texts/").size()));
  require(index < document.texts_size(), "reference " + ref + " names an arena slot");
  const auto& item = document.texts(index);
  switch (item.item_case()) {
    case docv1::BaseTextItem::kText: return item.text().base();
    case docv1::BaseTextItem::kSectionHeader: return item.section_header().base();
    case docv1::BaseTextItem::kListItem: return item.list_item().base();
    default: throw std::runtime_error("unexpected item kind at " + ref);
  }
}

std::vector<std::string> refs_of(const docv1::GroupItem& group) {
  std::vector<std::string> refs;
  for (const auto& child : group.children()) refs.push_back(child.ref());
  return refs;
}

bool contains(const std::vector<std::string>& refs, const std::string& ref) {
  for (const auto& have : refs) {
    if (have == ref) return true;
  }
  return false;
}

// Every reference the body and furniture trees hold names an arena item
// whose self_ref agrees with it.
void require_tree_resolves(const docv1::Document& document) {
  for (const auto* group : {&document.body(), &document.furniture()}) {
    for (const auto& child : group->children()) {
      if (!child.ref().starts_with("#/texts/")) continue;
      require(base_at(document, child.ref()).self_ref() == child.ref(),
              "tree reference " + child.ref() + " resolves to an item that agrees");
    }
  }
}

bool same(const docv1::Document& left, const docv1::Document& right) {
  return google::protobuf::util::MessageDifferencer::Equals(left, right);
}

// A five-page report with a running header on four pages and a body
// paragraph on every page.
docv1::Document report_with_running_header() {
  docv1::Document document = base_document();
  add_pages(&document, 5);
  for (int page = 1; page <= 5; ++page) {
    if (page <= 4) add_prose(&document, "ACME Quarterly Report", page, 20.0, 40.0);
    add_prose(&document, "Body paragraph " + std::to_string(page) + ".", page, 300.0, 320.0);
  }
  return document;
}

void verify_running_header_demoted() {
  docv1::Document document = report_with_running_header();
  std::vector<std::string> patterns;
  const int demoted = grparse::demote_running_furniture(&document, {}, &patterns);
  require(demoted == 4, "the header on four of five pages is demoted");
  require(patterns == std::vector<std::string>{"acme quarterly report"},
          "the report names the normalized header");
  const std::vector<std::string> furniture = refs_of(document.furniture());
  require(furniture == std::vector<std::string>{"#/texts/0", "#/texts/2", "#/texts/4", "#/texts/6"},
          "the furniture tree holds the headers in page order");
  for (const auto& ref : furniture) {
    const auto& base = base_at(document, ref);
    require(base.label() == docv1::DOC_ITEM_LABEL_PAGE_HEADER, "a top-band item becomes a header");
    require(base.content_layer() == docv1::CONTENT_LAYER_FURNITURE, "demoted items carry the furniture layer");
    require(base.parent().ref() == "#/furniture", "demoted items re-parent to the furniture group");
  }
  require(document.body().children_size() == 5, "the body keeps the five paragraphs");
  require(document.texts_size() == 9, "demotion retires nothing from the arena");
  require(!contains(refs_of(document.body()), "#/texts/0"), "the body no longer lists a header");
  require_tree_resolves(document);
}

void verify_page_numbers_demoted() {
  docv1::Document document = base_document();
  add_pages(&document, 3);
  add_prose(&document, "1", 1, 970.0, 990.0);
  add_prose(&document, "- 2 -", 2, 970.0, 990.0);
  add_prose(&document, "Page 3 of 3", 3, 970.0, 990.0);
  add_prose(&document, "iv", 2, 10.0, 30.0);
  add_prose(&document, "Chapter 3 begins here.", 2, 400.0, 420.0);
  std::vector<std::string> patterns;
  const int demoted = grparse::demote_running_furniture(&document, {}, &patterns);
  require(demoted == 4, "each standalone page number is demoted although none recurs");
  require(patterns == std::vector<std::string>{"<page number>"},
          "page numbers report as one pattern");
  for (const auto& ref : {"#/texts/0", "#/texts/1", "#/texts/2"}) {
    require(base_at(document, ref).label() == docv1::DOC_ITEM_LABEL_PAGE_FOOTER,
            "a bottom-band page number becomes a footer");
  }
  require(base_at(document, "#/texts/3").label() == docv1::DOC_ITEM_LABEL_PAGE_HEADER,
          "a top-band roman numeral becomes a header");
  require(refs_of(document.body()) == std::vector<std::string>{"#/texts/4"},
          "the paragraph mentioning a number outside the bands stays in the body");
  require(refs_of(document.furniture()) ==
              std::vector<std::string>{"#/texts/0", "#/texts/1", "#/texts/3", "#/texts/2"},
          "furniture lands in page order, body order within a page");
}

void verify_section_header_in_band_not_demoted() {
  docv1::Document document = base_document();
  add_pages(&document, 4);
  for (int page = 1; page <= 4; ++page) {
    add_section_header(&document, "Introduction", page, 20.0, 40.0);
    add_prose(&document, "Notes", page, 500.0, 520.0);
    add_prose(&document, "Body " + std::to_string(page) + ".", page, 300.0, 320.0);
  }
  const docv1::Document before = document;
  const grparse::RepairReport report = grparse::repair_document(&document);
  require(report.furniture_demoted == 0,
          "neither a repeated section header in the band nor repeated mid-page prose is demoted");
  require(same(before, document), "nothing else changed");
}

void verify_single_page_untouched() {
  docv1::Document document = base_document();
  add_pages(&document, 1);
  add_prose(&document, "Page 1", 1, 970.0, 990.0);
  add_prose(&document, "Only page.", 1, 300.0, 320.0);
  const docv1::Document before = document;
  const grparse::RepairReport report = grparse::repair_document(&document);
  require(!report.changed_anything(), "a single-page document has no running furniture");
  require(same(before, document), "a single-page document is left as it was");
}

void verify_normalization_and_page_number_shapes() {
  require(grparse::normalize_running_text("  Page  12 \n of 40 ") == "page # of #",
          "normalization folds case, collapses whitespace, and replaces digit runs");
  require(grparse::normalize_running_text("Page 3") == grparse::normalize_running_text("PAGE 14"),
          "page numbers on different pages normalize alike");
  for (const auto* shape : {"#", "- # -", "page # of #", "iv", "p. #", "# / #", "[#]"}) {
    require(grparse::is_page_number_shape(shape), std::string("page number shape: ") + shape);
  }
  for (const auto* shape : {"chapter #", "# apples", "", "introduction", "table #: results"}) {
    require(!grparse::is_page_number_shape(shape), std::string("not a page number: ") + shape);
  }
}

void verify_hyphen_rejoin() {
  grparse::HyphenationCounts counts;
  require(grparse::rejoin_hyphenated_words("infor-\nmation", &counts) == "information",
          "a line-break hyphen inside a word is rejoined");
  require(grparse::rejoin_hyphenated_words("infor- mation") == "information",
          "the single space a line join left counts as the break");
  require(grparse::rejoin_hyphenated_words("well-\nknown") == "well-known",
          "a known compound keeps its hyphen");
  require(grparse::rejoin_hyphenated_words("self- aware") == "self-aware",
          "self- keeps its hyphen before any letter");
  require(grparse::rejoin_hyphenated_words("re-\nenter") == "re-enter",
          "re- keeps its hyphen before a vowel");
  require(grparse::rejoin_hyphenated_words("re-\nmain") == "remain",
          "re- before a consonant is a broken word");
  require(grparse::rejoin_hyphenated_words("pre-\nvious") == "previous",
          "pre- before a consonant is a broken word");
  require(grparse::rejoin_hyphenated_words("Hong-\nKong") == "Hong-\nKong",
          "an uppercase second fragment is not rejoined");
  require(grparse::rejoin_hyphenated_words("42-\nyear") == "42-\nyear",
          "a numeric first fragment is not rejoined");
  require(grparse::rejoin_hyphenated_words("x - y") == "x - y",
          "a spaced dash is not a line-break hyphen");
  require(grparse::rejoin_hyphenated_words("end-\n") == "end-\n",
          "a hyphen with nothing after the break stays");
  require(grparse::rejoin_hyphenated_words("exam\xC2\xAD\nple") == "example",
          "a soft hyphen on a line break joins the word");
  require(grparse::rejoin_hyphenated_words("soft\xC2\xADhyphen", &counts) == "softhyphen",
          "an inline soft hyphen is removed");
  require(counts.rejoined == 1 && counts.soft_hyphens_removed == 1,
          "counts accumulate across calls");

  docv1::Document document = base_document();
  add_pages(&document, 1);
  const std::string prose =
      add_prose(&document, "The infor-\nmation was well-\nknown.", 1, 300.0, 320.0);
  const std::string header =
      add_section_header(&document, "Intro-\nduction", 1, 100.0, 120.0);
  const grparse::HyphenationCounts document_counts = grparse::rejoin_hyphenation(&document);
  require(document_counts.rejoined == 2, "both hyphens in the paragraph are visited");
  require(base_at(document, prose).text() == "The information was well-known.",
          "the paragraph reads as words");
  require(base_at(document, header).text() == "Intro-\nduction",
          "a section header is not prose and is left alone");
}

void verify_continuation_merged_across_pages() {
  docv1::Document document = base_document();
  add_pages(&document, 2);
  add_section_header(&document, "Minutes", 1, 100.0, 120.0);
  const std::string head =
      add_prose(&document, "The committee decided that the", 1, 900.0, 920.0);
  add_prose(&document, "budget would be approved.", 2, 100.0, 120.0);
  const std::string later = add_prose(&document, "Next item.", 2, 200.0, 220.0);
  auto* table = document.add_tables();
  table->set_self_ref("#/tables/0");
  table->mutable_parent()->set_ref("#/body");
  table->add_comments()->set_ref(later);
  document.mutable_body()->add_children()->set_ref("#/tables/0");
  auto* tail_base = document.mutable_texts(2)->mutable_text()->mutable_base();
  tail_base->add_source()->mutable_collector()->set_collector("pdf");
  (*tail_base->mutable_meta()->mutable_custom_fields())["font"].set_string_value("serif");

  const int merged = grparse::merge_continuations(&document, {});
  require(merged == 1, "one continuation merges");
  require(document.texts_size() == 3, "the absorbed item leaves the arena");
  const auto& merged_base = base_at(document, head);
  require(merged_base.text() == "The committee decided that the budget would be approved.",
          "texts join with one space");
  require(merged_base.prov_size() == 2 && merged_base.prov(0).page_no() == 1 &&
              merged_base.prov(1).page_no() == 2,
          "the tail's provenance appends to the head's");
  require(merged_base.source_size() == 1 && merged_base.source(0).collector().collector() == "pdf",
          "the tail's source carries over");
  require(merged_base.meta().custom_fields().at("font").string_value() == "serif",
          "the tail's custom fields carry over");
  require(refs_of(document.body()) ==
              std::vector<std::string>{"#/texts/0", "#/texts/1", "#/texts/2", "#/tables/0"},
          "the body tree is renumbered");
  require(base_at(document, "#/texts/2").text() == "Next item.",
          "the later item moved down one slot");
  require(document.tables(0).comments(0).ref() == "#/texts/2",
          "a reference into the later item follows the renumbering");
  require_tree_resolves(document);
}

void verify_continuation_applies_hyphen_rule() {
  docv1::Document document = base_document();
  add_pages(&document, 2);
  const std::string head = add_prose(&document, "All the infor-", 1, 900.0, 920.0);
  add_prose(&document, "mation flows.", 2, 100.0, 120.0);
  const std::string compound_head = add_prose(&document, "It is well-", 2, 900.0, 920.0);
  add_prose(&document, "known here.", 3, 100.0, 120.0);
  (*document.mutable_pages())[3].set_page_no(3);
  (*document.mutable_pages())[3].mutable_size()->set_height(1000.0);
  require(grparse::merge_continuations(&document, {}) == 2, "both pairs merge");
  require(base_at(document, head).text() == "All the information flows.",
          "a hyphen at the break joins the word");
  require(base_at(document, "#/texts/1").text() == "It is well-known here.",
          "a known compound keeps its hyphen across the break");
}

void verify_continuation_merges_within_a_page() {
  docv1::Document document = base_document();
  add_pages(&document, 1);
  const std::string head = add_prose(&document, "Column one ends mid", 1, 900.0, 920.0);
  add_prose(&document, "sentence in column two.", 1, 100.0, 120.0);
  auto* second_column = document.mutable_texts(1)->mutable_text()->mutable_base()->mutable_prov(0)->mutable_bbox();
  second_column->set_l(420.0);
  second_column->set_r(740.0);
  require(grparse::merge_continuations(&document, {}) == 1, "a column break merges");
  require(base_at(document, head).text() == "Column one ends mid sentence in column two.",
          "same-page neighbours join");
}

void verify_continuation_not_merged() {
  {
    docv1::Document document = base_document();
    add_pages(&document, 2);
    add_prose(&document, "The committee decided that the", 1, 900.0, 920.0);
    add_prose(&document, "Budget was a separate matter.", 2, 100.0, 120.0);
    require(grparse::merge_continuations(&document, {}) == 0,
            "an uppercase start is a new paragraph");
  }
  {
    docv1::Document document = base_document();
    add_pages(&document, 2);
    add_prose(&document, "The agenda covered", 1, 900.0, 920.0);
    add_list_item(&document, "budget approval", 2, 100.0, 120.0);
    require(grparse::merge_continuations(&document, {}) == 0, "a list item never merges");
  }
  {
    docv1::Document document = base_document();
    add_pages(&document, 2);
    add_prose(&document, "The committee decided that the", 1, 900.0, 920.0);
    add_section_header(&document, "Budget", 2, 50.0, 70.0);
    add_prose(&document, "budget would be approved.", 2, 100.0, 120.0);
    require(grparse::merge_continuations(&document, {}) == 0,
            "a section header between two paragraphs keeps them apart");
  }
  {
    docv1::Document document = base_document();
    add_pages(&document, 3);
    add_prose(&document, "The committee decided that the", 1, 900.0, 920.0);
    add_prose(&document, "budget would be approved.", 3, 100.0, 120.0);
    require(grparse::merge_continuations(&document, {}) == 0,
            "a paragraph two pages on is not a continuation");
  }
  {
    docv1::Document document = base_document();
    add_pages(&document, 2);
    add_prose(&document, "The meeting closed.", 1, 900.0, 920.0);
    add_prose(&document, "any other business followed.", 2, 100.0, 120.0);
    require(grparse::merge_continuations(&document, {}) == 0,
            "terminal punctuation ends the paragraph");
  }
  {
    docv1::Document document = base_document();
    add_pages(&document, 2);
    add_prose(&document, "one", 1, 2.0, 12.0);
    add_prose(&document, "two", 2, 2.0, 12.0);
    require(grparse::merge_continuations(&document, {}) == 0,
            "a paragraph that stopped in the upper half of its page did not run out of page");
  }
  {
    docv1::Document document = base_document();
    add_pages(&document, 1);
    add_prose(&document, "the first line", 1, 300.0, 320.0);
    add_prose(&document, "the second line below it", 1, 340.0, 360.0);
    require(grparse::merge_continuations(&document, {}) == 0,
            "a lower neighbour in the same column is not a column break");
  }
  {
    docv1::Document document = base_document();
    add_pages(&document, 3);
    add_prose(&document, "line one", 1, 900.0, 920.0);
    add_prose(&document, "line two", 2, 900.0, 920.0);
    add_prose(&document, "line three", 3, 100.0, 120.0);
    grparse::RepairOptions options;
    options.maximum_continuation_merges = 1;
    require(grparse::merge_continuations(&document, options) == 1, "the cap bounds a pass");
    require(document.texts_size() == 2, "only the capped merge happened");
  }
}

void verify_pass_is_idempotent() {
  docv1::Document document = report_with_running_header();
  for (int page = 1; page <= 5; ++page) {
    add_prose(&document, std::to_string(page), page, 970.0, 990.0);
  }
  const std::string head = add_prose(&document, "Every infor-\nmation item was", 4, 800.0, 820.0);
  add_prose(&document, "carried over to the next page.", 5, 100.0, 120.0);
  const grparse::RepairReport first = grparse::repair_document(&document);
  require(first.furniture_demoted == 9, "four headers and five page numbers are demoted");
  require(first.paragraphs_merged == 1 && first.hyphens_rejoined == 1,
          "the continuation merges and the hyphen rejoins");
  require(base_at(document, head).text() ==
              "Every information item was carried over to the next page.",
          "the merged paragraph reads as prose");
  const docv1::Document once = document;
  const grparse::RepairReport second = grparse::repair_document(&document);
  require(!second.changed_anything(), "a second pass finds nothing");
  require(same(once, document), "a second pass changes nothing");
  require_tree_resolves(document);
}

void verify_totals_accumulate() {
  docv1::Document document = report_with_running_header();
  const grparse::RepairTotals before = grparse::repair_totals();
  grparse::RepairOptions options;
  const grparse::RepairReport report = grparse::run_repair_pass(&document, options);
  const grparse::RepairTotals after = grparse::repair_totals();
  require(after.furniture_demoted - before.furniture_demoted ==
              static_cast<uint64_t>(report.furniture_demoted),
          "the process-wide totals grow by the report");
  options.demote_running_furniture = false;
  docv1::Document untouched = report_with_running_header();
  require(!grparse::run_repair_pass(&untouched, options).changed_anything(),
          "a disabled repair does nothing");
}

}  // namespace

int main() {
  try {
    verify_running_header_demoted();
    verify_page_numbers_demoted();
    verify_section_header_in_band_not_demoted();
    verify_single_page_untouched();
    verify_normalization_and_page_number_shapes();
    verify_hyphen_rejoin();
    verify_continuation_merged_across_pages();
    verify_continuation_applies_hyphen_rule();
    verify_continuation_merges_within_a_page();
    verify_continuation_not_merged();
    verify_pass_is_idempotent();
    verify_totals_accumulate();
  } catch (const std::exception& error) {
    std::println(stderr, "document-repair-test: {}", error.what());
    return 1;
  }
  std::println("document-repair-test: ok");
  return 0;
}
