// Anti-drift: the repair pass settles, and it settles in the same place.
//
// The single-repair tests next door each prove one repair on a document built
// to exercise it. This one runs every repair at once over one document that
// needs all of them, because the repairs feed each other: furniture demotion
// changes who is a body neighbour, the splits create items the heading pass
// then levels, the ordering pass moves the neighbours the continuation merge
// looks at. What a composite must guarantee is that the fixed point is
// reached in one pass, is the same fixed point whatever order the collector
// reported the items in, and is the one pinned below.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <google/protobuf/util/message_differencer.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"
#include "grparse/document_repair.h"
#include "grparse/heading_hierarchy.h"
#include "grparse/paragraph_split.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// One body item as a text-layer collector reports it.
struct Item {
  std::string text;
  int page = 1;
  double top = 0;
  double bottom = 0;
  docv1::DocItemLabel label = docv1::DOC_ITEM_LABEL_TEXT;
};

docv1::Document base_document(int pages) {
  docv1::Document document;
  document.set_name("composite.pdf");
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  for (int page = 1; page <= pages; ++page) {
    auto& item = (*document.mutable_pages())[page];
    item.set_page_no(page);
    item.mutable_size()->set_width(800);
    item.mutable_size()->set_height(1000);
  }
  return document;
}

void add_item(docv1::Document* document, const Item& item) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  docv1::TextItemBase* base = nullptr;
  if (item.label == docv1::DOC_ITEM_LABEL_SECTION_HEADER) {
    base = document->add_texts()->mutable_section_header()->mutable_base();
  } else {
    base = document->add_texts()->mutable_text()->mutable_base();
  }
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_label(item.label);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(item.text);
  base->set_orig(item.text);
  auto* prov = base->add_prov();
  prov->set_page_no(item.page);
  auto* box = prov->mutable_bbox();
  box->set_l(60);
  box->set_r(740);
  box->set_t(item.top);
  box->set_b(item.bottom);
  box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  base->add_source()->mutable_collector()->set_collector("pdf");
  document->mutable_body()->add_children()->set_ref(ref);
}

// A four-page report that needs every repair: a running header and a page
// number on each page, a numbered heading run into its paragraph, a form
// block folded into one item, a paragraph a page break cut in two with a
// hyphenated word at the cut, and headings whose depth is only readable
// against each other.
std::vector<Item> report_items() {
  return {
      {"Field Report", 1, 60, 130, docv1::DOC_ITEM_LABEL_SECTION_HEADER},
      {"Survey of the north traverse", 1, 200, 240},
      {"1. Method", 1, 300, 330, docv1::DOC_ITEM_LABEL_SECTION_HEADER},
      {"The instruments were calibrated at dawn.", 1, 360, 420},
      {"Field Report", 1, 10, 30},
      {"1", 1, 950, 980},

      {"1.1 Traverse", 2, 100, 130, docv1::DOC_ITEM_LABEL_SECTION_HEADER},
      {"Each leg was measured twice and the mean recor-", 2, 700, 760},
      {"Field Report", 2, 10, 30},
      {"2", 2, 950, 980},

      {"ded in the field book before the party moved on.", 3, 100, 160},
      {"2. INSTRUMENT LOG The total station was serviced before the survey began.",
       3, 300, 380},
      {"Field Report", 3, 10, 30},
      {"3", 3, 950, 980},

      {"\xE2\x98\x92 I confirm the instruments were returned. "
       "\xE2\x98\x90 I request an extension form. "
       "Signature: ______________ Date: ________",
       4, 200, 320},
      {"Field Report", 4, 10, 30},
      {"4", 4, 950, 980},
  };
}

docv1::Document build_report(const std::vector<Item>& items) {
  docv1::Document document = base_document(4);
  for (const Item& item : items) add_item(&document, item);
  return document;
}

const docv1::TextItemBase& base_of(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle: return item.title().base();
    case docv1::BaseTextItem::kSectionHeader: return item.section_header().base();
    case docv1::BaseTextItem::kListItem: return item.list_item().base();
    case docv1::BaseTextItem::kFormula: return item.formula().base();
    case docv1::BaseTextItem::kText: return item.text().base();
    default: throw std::runtime_error("unexpected text item arm");
  }
}

// The body read out as text, so a document built from a permuted report is
// still comparable with one built in the collector's own order.
std::vector<std::string> body_texts(const docv1::Document& document) {
  std::vector<std::string> out;
  for (const auto& child : document.body().children()) {
    const int index = std::stoi(child.ref().substr(std::string("#/texts/").size()));
    out.push_back(base_of(document.texts(index)).text());
  }
  return out;
}

std::string joined(const std::vector<std::string>& parts) {
  std::string out;
  for (const auto& part : parts) out += "[" + part + "]";
  return out;
}

// Heading text to level, so levels can be compared across permutations too.
std::map<std::string, int32_t> heading_levels(const docv1::Document& document) {
  std::map<std::string, int32_t> levels;
  for (const docv1::BaseTextItem& item : document.texts()) {
    if (item.has_section_header()) {
      levels[item.section_header().base().text()] = item.section_header().level();
    } else if (item.has_title()) {
      levels[item.title().base().text()] = 0;
    }
  }
  return levels;
}

// What one repair of the report produces, pinned so a change to any single
// repair shows up here as well as in that repair's own test.
void verify_pinned_composite_repair() {
  docv1::Document document = build_report(report_items());
  const grparse::RepairReport report = grparse::repair_document(&document);

  require(report.furniture_demoted == 8,
          "four running headers and four page numbers are furniture; demoted " +
              std::to_string(report.furniture_demoted));
  require(report.headings_split == 1, "the run-in heading is split out of its paragraph");
  require(report.form_rows_split == 3, "the form block becomes four rows");
  require(report.paragraphs_merged == 1, "the paragraph the page break cut is rejoined");
  require(report.titles_promoted == 1, "the tallest first-page heading becomes the title");
  require(report.heading_levels_assigned == 3, "the three numbered headings take a depth");
  // The hyphen at the cut is joined by the continuation merge itself, which
  // applies the same rule while it joins; the standalone rejoin pass then has
  // nothing left to do, so a nonzero count here would mean the merge stopped
  // applying it.
  require(report.hyphens_rejoined == 0,
          "the continuation merge already applied the hyphen rule");

  const std::vector<std::string> expected = {
      "Field Report",
      "Survey of the north traverse",
      "1. Method",
      "The instruments were calibrated at dawn.",
      "1.1 Traverse",
      "Each leg was measured twice and the mean recorded in the field book before the party "
      "moved on.",
      "2. INSTRUMENT LOG",
      "The total station was serviced before the survey began.",
      "\xE2\x98\x92 I confirm the instruments were returned.",
      "\xE2\x98\x90 I request an extension form.",
      "Signature: ______________",
      "Date: ________",
  };
  require(body_texts(document) == expected,
          "the repaired body reads as the report does; got " + joined(body_texts(document)));

  require(document.texts(0).has_title() &&
              document.texts(0).title().base().text() == "Field Report",
          "the promoted title is a TitleItem, not a section header");
  const std::map<std::string, int32_t> levels = heading_levels(document);
  require(levels.at("Field Report") == 0, "the report's own title leads");
  require(levels.at("1. Method") == 1 && levels.at("2. INSTRUMENT LOG") == 1,
          "top-level numbering is level 1");
  require(levels.at("1.1 Traverse") == 2, "1.1 is a level deeper than 1");
  require(document.furniture().children_size() == 8, "the furniture tree holds what was demoted");
}

// One pass reaches the fixed point: the second and third find nothing and
// move nothing, down to the canonical bytes.
void verify_the_pass_settles_in_one_run() {
  docv1::Document document = build_report(report_items());
  grparse::repair_document(&document);
  const std::string once = grparse::render_canonical_json(document);
  const docv1::Document settled = document;

  const grparse::RepairReport second = grparse::repair_document(&document);
  require(!second.changed_anything(), "a second repair found work to do");
  require(google::protobuf::util::MessageDifferencer::Equals(settled, document),
          "a second repair changed the document");
  const grparse::RepairReport third = grparse::repair_document(&document);
  require(!third.changed_anything(), "a third repair found work to do");
  require(grparse::render_canonical_json(document) == once,
          "a third repair moved the canonical bytes");
}

// The fixed point does not depend on the order the collector reported items
// in: the repairs read geometry and text, never arena position.
void verify_the_fixed_point_is_report_order_independent() {
  docv1::Document reference = build_report(report_items());
  grparse::repair_document(&reference);
  const std::vector<std::string> expected = body_texts(reference);
  const std::map<std::string, int32_t> expected_levels = heading_levels(reference);

  // A handful of adversarial report orders rather than every permutation of
  // seventeen items: a reversed page walk, every piece of furniture appended
  // after the body, and a walk sorted by top edge that interleaves the pages.
  std::vector<std::vector<Item>> orders;
  std::vector<Item> reversed = report_items();
  std::ranges::reverse(reversed);
  orders.push_back(reversed);

  std::vector<Item> furniture_last;
  std::vector<Item> furniture;
  for (const Item& item : report_items()) {
    const bool is_furniture = item.text == "Field Report" &&
                              item.label != docv1::DOC_ITEM_LABEL_SECTION_HEADER;
    const bool is_number =
        item.text.size() == 1 &&
        std::isdigit(static_cast<unsigned char>(item.text[0])) != 0;
    if (is_furniture || is_number) {
      furniture.push_back(item);
    } else {
      furniture_last.push_back(item);
    }
  }
  furniture_last.insert(furniture_last.end(), furniture.begin(), furniture.end());
  orders.push_back(furniture_last);

  std::vector<Item> by_top = report_items();
  std::ranges::stable_sort(by_top, [](const Item& a, const Item& b) { return a.top < b.top; });
  orders.push_back(by_top);

  for (std::size_t index = 0; index < orders.size(); ++index) {
    docv1::Document document = build_report(orders[index]);
    grparse::repair_document(&document);
    require(body_texts(document) == expected,
            "report order " + std::to_string(index) + " repaired to a different body: " +
                joined(body_texts(document)));
    require(heading_levels(document) == expected_levels,
            "report order " + std::to_string(index) + " repaired to different heading levels");
  }
}

// Each pass the composite depends on is idempotent on its own, so a caller
// that runs one of them directly (the streaming surface levels headings
// without the rest) gets the same guarantee.
void verify_each_pass_is_idempotent_on_its_own() {
  const std::vector<std::string> pdf = {"pdf"};

  docv1::Document headings = build_report(report_items());
  grparse::infer_heading_hierarchy(&headings);
  const docv1::Document levelled = headings;
  const grparse::HeadingReport again = grparse::infer_heading_hierarchy(&headings);
  require(again.levels_assigned == 0 && again.titles_merged == 0 && again.titles_promoted == 0 &&
              again.headings_demoted == 0,
          "a second heading pass reported work");
  require(google::protobuf::util::MessageDifferencer::Equals(levelled, headings),
          "a second heading pass changed the document");

  docv1::Document splits = build_report(report_items());
  require(grparse::split_run_in_headings(&splits, pdf) == 1, "one run-in heading is split");
  require(grparse::split_form_rows(&splits, pdf) == 3, "three form rows are split off");
  const docv1::Document split_once = splits;
  require(grparse::split_run_in_headings(&splits, pdf) == 0, "a second run-in split found work");
  require(grparse::split_form_rows(&splits, pdf) == 0, "a second form split found work");
  require(google::protobuf::util::MessageDifferencer::Equals(split_once, splits),
          "a second split pass changed the document");
}

}  // namespace

int main() {
  try {
    verify_pinned_composite_repair();
    verify_the_pass_settles_in_one_run();
    verify_the_fixed_point_is_report_order_independent();
    verify_each_pass_is_idempotent_on_its_own();
  } catch (const std::exception& error) {
    std::println(stderr, "repair-idempotence-test: {}", error.what());
    return EXIT_FAILURE;
  }
  std::println("repair-idempotence-test: ok");
  return EXIT_SUCCESS;
}
