// The geometry every pass after the merge reads: page heights, the top-down
// normalization of a provenance box, where an arena item sits, what label a
// reference names, and whose collectors produced a body.  Documents are hand
// built, so each case states the whole geometry it depends on.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_geometry.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;

using grparse_test::add_code;
using grparse_test::add_collector_source;
using grparse_test::add_group;
using grparse_test::add_page;
using grparse_test::add_paragraph;
using grparse_test::add_picture;
using grparse_test::add_prov;
using grparse_test::add_table;
using grparse_test::add_text;
using grparse_test::base_document;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

// The provenance list of the text item a reference names, for the writes a
// case makes after the builder placed the item.
google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* text_prov(docv1::Document* document,
                                                                     int index) {
  auto* item = document->mutable_texts(index);
  if (item->item_case() == docv1::BaseTextItem::kCode) return item->mutable_code()->mutable_prov();
  auto* base = grparse::mutable_text_base_of(item);
  require(base != nullptr, "the fixture's text item must carry a base");
  return base->mutable_prov();
}

void verify_page_heights_prefer_the_declared_size() {
  docv1::Document document = base_document("heights.pdf");
  add_page(&document, 1, 612, 792);
  add_paragraph(&document, "#/body", "on page one");
  add_prov(text_prov(&document, 0), 1, 10, 20, 110, 900);

  const std::map<int, double> heights = grparse::document_page_heights(document);
  require_equal(heights.size(), std::size_t{1}, "one page was declared");
  require_equal(heights.at(1), 792.0,
                "a page that states its own size keeps it, whatever an item reaches");
}

void verify_page_heights_fall_back_to_the_furthest_box_edge() {
  docv1::Document document = base_document("heights.pdf");
  add_paragraph(&document, "#/body", "unpaged producer");
  add_prov(text_prov(&document, 0), 3, 10, 100, 110, 400);
  auto* table = add_table(&document, "#/body");
  add_prov(table->mutable_prov(), 3, 10, 420, 110, 460);
  auto* picture = add_picture(&document, "#/body", "");
  add_prov(picture->mutable_prov(), 4, 0, 0, 50, 50);

  const std::map<int, double> heights = grparse::document_page_heights(document);
  require_equal(heights.at(3), 460.0, "an undeclared page is as tall as its furthest box edge");
  require_equal(heights.at(4), 50.0, "every page an item names gets a height");
}

void verify_page_heights_ignore_unnumbered_and_boxless_provenance() {
  docv1::Document document = base_document("heights.pdf");
  add_paragraph(&document, "#/body", "page zero");
  add_prov(text_prov(&document, 0), 0, 10, 10, 20, 20);
  add_paragraph(&document, "#/body", "no box");
  text_prov(&document, 1)->Add()->set_page_no(2);

  const std::map<int, double> heights = grparse::document_page_heights(document);
  require(heights.empty(), "a page number of zero and a boxless entry name no page height");
}

void verify_top_down_box_normalizes_both_origins() {
  docv1::BoundingBox top_left;
  top_left.set_l(100);
  top_left.set_t(50);
  top_left.set_r(10);
  top_left.set_b(20);
  top_left.set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  const grparse::TopDownBox down = grparse::top_down_box(top_left, 800);
  require_equal(down.left, 10.0, "the smaller horizontal edge is the left one");
  require_equal(down.right, 100.0, "the larger horizontal edge is the right one");
  require_equal(down.top, 20.0, "a top-left box measures downward already");
  require_equal(down.bottom, 50.0, "a top-left box keeps its lower edge");
  require_equal(down.width(), 90.0, "width is right minus left");
  require_equal(down.height(), 30.0, "height is bottom minus top");

  docv1::BoundingBox bottom_left;
  bottom_left.set_l(10);
  bottom_left.set_t(700);
  bottom_left.set_r(100);
  bottom_left.set_b(600);
  bottom_left.set_coord_origin(docv1::COORD_ORIGIN_BOTTOMLEFT);
  const grparse::TopDownBox flipped = grparse::top_down_box(bottom_left, 800);
  require_equal(flipped.top, 100.0, "the page height flips a bottom-left box's upper edge");
  require_equal(flipped.bottom, 200.0, "the page height flips a bottom-left box's lower edge");
  require_equal(flipped.height(), 100.0, "flipping preserves the box's height");
}

void verify_an_unset_origin_reads_as_top_left() {
  docv1::BoundingBox box;
  box.set_l(0);
  box.set_t(30);
  box.set_r(10);
  box.set_b(60);
  const grparse::TopDownBox down = grparse::top_down_box(box, 800);
  require_equal(down.top, 30.0, "an unset coordinate origin measures downward");
  require_equal(down.bottom, 60.0, "an unset coordinate origin keeps its lower edge");
}

void verify_first_page_of_takes_the_lowest_positive_page() {
  docv1::Document document = base_document("pages.pdf");
  add_paragraph(&document, "#/body", "spans pages");
  auto* prov = text_prov(&document, 0);
  add_prov(prov, 5, 0, 0, 10, 10);
  add_prov(prov, 2, 0, 0, 10, 10);
  prov->Add()->set_page_no(0);
  require_equal(grparse::first_page_of(*prov), 2, "the lowest page a provenance names wins");

  docv1::Document unpaged = base_document("pages.pdf");
  add_paragraph(&unpaged, "#/body", "unplaced");
  require_equal(grparse::first_page_of(*text_prov(&unpaged, 0)), 0,
                "an item with no provenance names no page");
}

void verify_item_placement_unions_the_boxes_on_the_first_page() {
  docv1::Document document = base_document("placement.pdf");
  add_page(&document, 1, 612, 792);
  add_page(&document, 2, 612, 792);
  add_paragraph(&document, "#/body", "two columns on page one, a tail on page two");
  auto* prov = text_prov(&document, 0);
  add_prov(prov, 1, 50, 100, 150, 200);
  add_prov(prov, 1, 300, 80, 400, 160);
  add_prov(prov, 2, 0, 0, 600, 700);

  const std::map<int, double> heights = grparse::document_page_heights(document);
  const auto placement = grparse::item_placement(document, "#/texts/0", heights);
  require(placement.has_value(), "an item with a box on a known page has a placement");
  require_equal(placement->page, 1, "the placement sits on the item's first page");
  require_equal(placement->box.left, 50.0, "the union takes the leftmost edge");
  require_equal(placement->box.top, 80.0, "the union takes the highest edge");
  require_equal(placement->box.right, 400.0, "the union takes the rightmost edge");
  require_equal(placement->box.bottom, 200.0, "the union takes the lowest edge");
  require_equal(placement->box.width(), 350.0, "the union spans both columns");
  require_equal(placement->box.height(), 120.0, "the union spans both rows");
}

void verify_placement_is_absent_without_page_box_or_area() {
  docv1::Document document = base_document("placement.pdf");
  add_page(&document, 1, 612, 792);
  const std::map<int, double> heights = grparse::document_page_heights(document);

  add_paragraph(&document, "#/body", "no provenance at all");
  require(!grparse::item_placement(document, "#/texts/0", heights).has_value(),
          "an item without provenance has no placement");

  add_paragraph(&document, "#/body", "a box with no area");
  add_prov(text_prov(&document, 1), 1, 40, 40, 40, 90);
  require(!grparse::item_placement(document, "#/texts/1", heights).has_value(),
          "a zero-width box does not place an item");

  add_paragraph(&document, "#/body", "a page nothing declares");
  text_prov(&document, 2)->Add()->set_page_no(9);
  require(!grparse::item_placement(document, "#/texts/2", heights).has_value(),
          "a page with no known height does not place an item");

  require(!grparse::item_placement(document, "#/texts/99", heights).has_value(),
          "a reference past the arena has no placement");
  require(!grparse::item_placement(document, "not-a-ref", heights).has_value(),
          "an unparseable reference has no placement");
  require(!grparse::item_placement(document, "#/body", heights).has_value(),
          "the body root is not an arena item");
}

void verify_a_groups_placement_is_its_childrens() {
  docv1::Document document = base_document("groups.pdf");
  add_page(&document, 1, 612, 792);
  add_page(&document, 2, 612, 792);
  const std::string list = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_text(&document, list, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "first");
  add_prov(text_prov(&document, 0), 1, 60, 300, 200, 320);
  add_text(&document, list, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "second");
  add_prov(text_prov(&document, 1), 1, 60, 330, 240, 350);

  const std::map<int, double> heights = grparse::document_page_heights(document);
  const auto placement = grparse::item_placement(document, list, heights);
  require(placement.has_value(), "a group with placed children has a placement");
  require_equal(placement->page, 1, "the group sits on its children's page");
  require_equal(placement->box.top, 300.0, "the group reaches its first child's top");
  require_equal(placement->box.bottom, 350.0, "the group reaches its last child's bottom");
  require_equal(placement->box.right, 240.0, "the group is as wide as its widest child");
}

void verify_a_group_takes_the_lowest_page_its_children_reach() {
  docv1::Document document = base_document("groups.pdf");
  add_page(&document, 1, 612, 792);
  add_page(&document, 2, 612, 792);
  const std::string list = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_text(&document, list, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "on page two");
  add_prov(text_prov(&document, 0), 2, 60, 100, 200, 120);
  add_text(&document, list, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "on page one");
  add_prov(text_prov(&document, 1), 1, 60, 700, 200, 720);

  const std::map<int, double> heights = grparse::document_page_heights(document);
  const auto placement = grparse::item_placement(document, list, heights);
  require(placement.has_value(), "a group spanning pages still has a placement");
  require_equal(placement->page, 1, "the group belongs to the lowest page its children reach");
  require_equal(placement->box.top, 700.0, "only that page's children shape the box");
}

void verify_a_nested_group_folds_into_its_parent() {
  docv1::Document document = base_document("groups.pdf");
  add_page(&document, 1, 612, 792);
  const std::string outer = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  const std::string inner = add_group(&document, outer, docv1::GROUP_LABEL_LIST);
  add_text(&document, inner, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "nested");
  add_prov(text_prov(&document, 0), 1, 80, 200, 300, 220);

  const std::map<int, double> heights = grparse::document_page_heights(document);
  const auto placement = grparse::item_placement(document, outer, heights);
  require(placement.has_value(), "a group placed only through a nested group still places");
  require_equal(placement->box.left, 80.0, "the nested child's box reaches the outer group");
}

void verify_provenance_placement_reads_a_list_in_hand() {
  docv1::Document document = base_document("placement.pdf");
  add_page(&document, 1, 400, 500);
  auto* table = add_table(&document, "#/body");
  add_prov(table->mutable_prov(), 1, 10, 40, 390, 90, docv1::COORD_ORIGIN_BOTTOMLEFT);

  const std::map<int, double> heights = grparse::document_page_heights(document);
  const auto placement = grparse::provenance_placement(table->prov(), heights);
  require(placement.has_value(), "a provenance list in hand places on its own");
  require_equal(placement->box.top, 410.0, "the bottom-left box flips against the page height");
  require_equal(placement->box.bottom, 460.0, "the flipped box keeps its height");
}

void verify_item_label_reads_each_arena() {
  docv1::Document document = base_document("labels.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kTitle, docv1::DOC_ITEM_LABEL_TITLE, "Title");
  add_code(&document, "#/body", "puts 1", docv1::CODE_LANGUAGE_LABEL_RUBY);
  const std::string group = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_table(&document, "#/body");
  add_picture(&document, "#/body", "");

  require_equal(static_cast<int>(grparse::item_label(document, "#/texts/0")),
                static_cast<int>(docv1::DOC_ITEM_LABEL_TITLE), "a text item reports its own label");
  require_equal(static_cast<int>(grparse::item_label(document, "#/texts/1")),
                static_cast<int>(docv1::DOC_ITEM_LABEL_CODE),
                "a code item reports its inlined label");
  require_equal(static_cast<int>(grparse::item_label(document, "#/tables/0")),
                static_cast<int>(docv1::DOC_ITEM_LABEL_TABLE), "the tables arena is TABLE");
  require_equal(static_cast<int>(grparse::item_label(document, "#/pictures/0")),
                static_cast<int>(docv1::DOC_ITEM_LABEL_PICTURE), "the pictures arena is PICTURE");
  require_equal(static_cast<int>(grparse::item_label(document, group)),
                static_cast<int>(docv1::DOC_ITEM_LABEL_UNSPECIFIED), "a group has no item label");
  require_equal(static_cast<int>(grparse::item_label(document, "#/texts/44")),
                static_cast<int>(docv1::DOC_ITEM_LABEL_UNSPECIFIED),
                "a reference past the arena has no item label");
}

void verify_produced_only_by_needs_every_collector_named() {
  docv1::Document document = base_document("sources.pdf");
  add_paragraph(&document, "#/body", "ocr text");
  add_collector_source(grparse::mutable_text_base_of(document.mutable_texts(0))->mutable_source(),
                       "grparse");
  auto* table = add_table(&document, "#/body");
  add_collector_source(table->mutable_source(), "grparse");

  require(grparse::produced_only_by(document, {"grparse"}),
          "a body from one named collector is produced only by it");
  require(grparse::produced_only_by(document, {"grparse", "libreoffice"}),
          "a superset of the collectors in the body still holds");
  require(!grparse::produced_only_by(document, {"libreoffice"}),
          "a collector the body used but the list omits breaks the claim");

  auto* picture = add_picture(&document, "#/body", "");
  add_collector_source(picture->mutable_source(), "libreoffice");
  require(!grparse::produced_only_by(document, {"grparse"}),
          "one item from another collector breaks the claim");
}

void verify_produced_only_by_is_false_without_attribution() {
  docv1::Document document = base_document("sources.pdf");
  add_paragraph(&document, "#/body", "unattributed");
  require(!grparse::produced_only_by(document, {"grparse"}),
          "a body that names no collector at all is not produced only by one");
  require(!grparse::produced_only_by(base_document("empty.pdf"), {"grparse"}),
          "an empty document is not produced only by any collector");
}

void verify_text_base_of_covers_every_variant() {
  docv1::Document document = base_document("variants.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kTitle, docv1::DOC_ITEM_LABEL_TITLE, "t");
  add_text(&document, "#/body", docv1::BaseTextItem::kSectionHeader,
           docv1::DOC_ITEM_LABEL_SECTION_HEADER, "h", 2);
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "l");
  add_text(&document, "#/body", docv1::BaseTextItem::kFormula, docv1::DOC_ITEM_LABEL_FORMULA, "f");
  add_paragraph(&document, "#/body", "p");
  add_code(&document, "#/body", "c", docv1::CODE_LANGUAGE_LABEL_C);

  for (int index = 0; index < 5; ++index) {
    const auto* base = grparse::text_base_of(document.texts(index));
    require(base != nullptr, "every nesting text variant has a base");
    require_equal(base->self_ref(), "#/texts/" + std::to_string(index),
                  "the base carries the item's own reference");
  }
  require(grparse::text_base_of(document.texts(5)) == nullptr,
          "a code item inlines its fields and has no base");
  require(grparse::mutable_text_base_of(document.mutable_texts(5)) == nullptr,
          "the mutable accessor agrees with the const one on a code item");

  docv1::BaseTextItem unset;
  require(grparse::text_base_of(unset) == nullptr, "an unset text variant has no base");
  require(grparse::mutable_text_base_of(&unset) == nullptr,
          "the mutable accessor agrees on an unset variant");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("document-geometry-test", "ok", {
      verify_page_heights_prefer_the_declared_size,
      verify_page_heights_fall_back_to_the_furthest_box_edge,
      verify_page_heights_ignore_unnumbered_and_boxless_provenance,
      verify_top_down_box_normalizes_both_origins,
      verify_an_unset_origin_reads_as_top_left,
      verify_first_page_of_takes_the_lowest_positive_page,
      verify_item_placement_unions_the_boxes_on_the_first_page,
      verify_placement_is_absent_without_page_box_or_area,
      verify_a_groups_placement_is_its_childrens,
      verify_a_group_takes_the_lowest_page_its_children_reach,
      verify_a_nested_group_folds_into_its_parent,
      verify_provenance_placement_reads_a_list_in_hand,
      verify_item_label_reads_each_arena,
      verify_produced_only_by_needs_every_collector_named,
      verify_produced_only_by_is_false_without_attribution,
      verify_text_base_of_covers_every_variant,
  });
}
