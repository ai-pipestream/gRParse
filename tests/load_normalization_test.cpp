// The load-time normalizations every export renderer reproduces before it
// reads a document: provenance boxes clamped to their page, ordered-list
// groups relabelled, and list items parented outside a list group moved into
// a synthesized one with every reference renumbered.  Each case states a
// document before and asserts the document after.

#include <string>

#include "../src/render/load_normalization.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;
namespace render = grparse::render;

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

docv1::TextItemBase* base_of(docv1::Document* document, int index) {
  auto* item = document->mutable_texts(index);
  switch (item->item_case()) {
    case docv1::BaseTextItem::kListItem: return item->mutable_list_item()->mutable_base();
    case docv1::BaseTextItem::kText: return item->mutable_text()->mutable_base();
    default: break;
  }
  require(false, "the fixture's text item must be a list item or a plain text item");
  return nullptr;
}

const docv1::TextItemBase& const_base_of(const docv1::Document& document, int index) {
  const auto& item = document.texts(index);
  if (item.item_case() == docv1::BaseTextItem::kListItem) return item.list_item().base();
  return item.text().base();
}

std::string children_of(const google::protobuf::RepeatedPtrField<docv1::RefItem>& children) {
  std::string joined;
  for (const auto& child : children) {
    if (!joined.empty()) joined += ", ";
    joined += child.ref();
  }
  return joined;
}

// ---- clamping -------------------------------------------------------------

void verify_a_document_inside_its_pages_needs_no_clamping() {
  docv1::Document document = base_document("clean.pdf");
  add_page(&document, 1, 612, 792);
  add_paragraph(&document, "#/body", "inside the page");
  add_prov(base_of(&document, 0)->mutable_prov(), 1, 10, 20, 300, 60);

  require(!render::needs_clamping(document), "a document inside its pages needs no clamping");
  const std::string before = document.SerializeAsString();
  render::clamp_document(&document);
  require_equal(document.SerializeAsString(), before,
                "clamping a document already inside its pages changes nothing");
}

void verify_boxes_outside_their_page_are_pulled_back() {
  docv1::Document document = base_document("overflow.pdf");
  add_page(&document, 1, 612, 792);
  add_paragraph(&document, "#/body", "over the edges");
  add_prov(base_of(&document, 0)->mutable_prov(), 1, -10, -5, 700, 900);

  require(render::needs_clamping(document), "a box past its page needs clamping");
  render::clamp_document(&document);
  const docv1::BoundingBox& box = const_base_of(document, 0).prov(0).bbox();
  require_equal(box.l(), 0.0, "a negative left edge clamps to the page's left edge");
  require_equal(box.t(), 0.0, "a negative top edge clamps to the page's top edge");
  require_equal(box.r(), 612.0, "a right edge past the page clamps to the page width");
  require_equal(box.b(), 792.0, "a bottom edge past the page clamps to the page height");
  require(!render::needs_clamping(document), "clamping is a fixed point");
}

void verify_pictures_and_tables_clamp_too() {
  docv1::Document document = base_document("overflow.pdf");
  add_page(&document, 1, 100, 200);
  auto* picture = add_picture(&document, "#/body", "");
  add_prov(picture->mutable_prov(), 1, 0, 0, 500, 500);
  auto* table = add_table(&document, "#/body");
  add_prov(table->mutable_prov(), 1, 0, 0, 500, 500);

  require(render::needs_clamping(document), "a picture past its page needs clamping");
  render::clamp_document(&document);
  require_equal(document.pictures(0).prov(0).bbox().r(), 100.0, "a picture box clamps to the page");
  require_equal(document.tables(0).prov(0).bbox().b(), 200.0, "a table box clamps to the page");
}

void verify_a_page_the_document_never_declares_is_left_alone() {
  docv1::Document document = base_document("unpaged.pdf");
  add_paragraph(&document, "#/body", "on a page nothing declares");
  add_prov(base_of(&document, 0)->mutable_prov(), 9, -100, -100, 9999, 9999);

  require(!render::needs_clamping(document),
          "a box on an undeclared page has nothing to clamp against");
  const std::string before = document.SerializeAsString();
  render::clamp_document(&document);
  require_equal(document.SerializeAsString(), before,
                "an undeclared page leaves its boxes untouched");
}

void verify_table_cells_clamp_only_when_the_table_sits_on_one_page() {
  docv1::Document single = base_document("cells.pdf");
  add_page(&single, 1, 100, 200);
  auto* table = add_table(&single, "#/body");
  add_prov(table->mutable_prov(), 1, 0, 0, 100, 200);
  auto* cell = table->mutable_data()->add_table_cells();
  cell->set_text("wide");
  cell->mutable_bbox()->set_l(-5);
  cell->mutable_bbox()->set_r(400);
  cell->mutable_bbox()->set_t(0);
  cell->mutable_bbox()->set_b(50);
  require(render::needs_clamping(single), "a cell box past its page needs clamping");
  render::clamp_document(&single);
  require_equal(single.tables(0).data().table_cells(0).bbox().r(), 100.0,
                "a single-page table clamps its cell boxes");

  docv1::Document spread = single;
  spread.mutable_tables(0)->mutable_data()->mutable_table_cells(0)->mutable_bbox()->set_r(400);
  add_page(&spread, 2, 100, 200);
  add_prov(spread.mutable_tables(0)->mutable_prov(), 2, 0, 0, 100, 200);
  require(!render::needs_clamping(spread),
          "a table spanning two pages has no single page to clamp its cells against");
  render::clamp_document(&spread);
  require_equal(spread.tables(0).data().table_cells(0).bbox().r(), 400.0,
                "a table spanning two pages leaves its cell boxes alone");
}

// ---- ordered-list relabelling --------------------------------------------

void verify_ordered_list_groups_are_relabelled_to_plain_lists() {
  docv1::Document document = base_document("ordered.md");
  const std::string ordered = add_group(&document, "#/body", docv1::GROUP_LABEL_ORDERED_LIST);
  add_text(&document, ordered, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "one", 0, true);

  require(render::has_ordered_list_groups(document), "an ordered-list group is detected");
  render::relabel_ordered_list_groups(&document);
  require_equal(static_cast<int>(document.groups(0).label()),
                static_cast<int>(docv1::GROUP_LABEL_LIST),
                "an ordered-list group becomes a plain list group");
  require(!render::has_ordered_list_groups(document), "relabelling is a fixed point");
  require(document.texts(0).list_item().enumerated(),
          "item numbering stays on the list item, not on its group");
}

void verify_the_roots_are_relabelled_as_well() {
  docv1::Document document = base_document("ordered.md");
  document.mutable_body()->set_label(docv1::GROUP_LABEL_ORDERED_LIST);
  document.mutable_furniture()->set_label(docv1::GROUP_LABEL_ORDERED_LIST);
  require(render::has_ordered_list_groups(document), "an ordered-list root is detected");
  render::relabel_ordered_list_groups(&document);
  require_equal(static_cast<int>(document.body().label()),
                static_cast<int>(docv1::GROUP_LABEL_LIST), "the body root is relabelled");
  require_equal(static_cast<int>(document.furniture().label()),
                static_cast<int>(docv1::GROUP_LABEL_LIST), "the furniture root is relabelled");
}

void verify_relabelling_must_run_before_the_migration() {
  docv1::Document document = base_document("ordered.md");
  const std::string ordered = add_group(&document, "#/body", docv1::GROUP_LABEL_ORDERED_LIST);
  add_text(&document, ordered, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "one", 0, true);

  require(render::has_misplaced_list_items(document),
          "an ordered-list group is not yet a list parent, so its items read as misplaced");
  render::relabel_ordered_list_groups(&document);
  require(!render::has_misplaced_list_items(document),
          "once relabelled, the same group is a legitimate list parent");
}

// ---- misplaced list items -------------------------------------------------

void verify_a_list_item_in_a_list_group_stays_put() {
  docv1::Document document = base_document("list.md");
  const std::string list = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_text(&document, list, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "alpha");

  require(!render::has_misplaced_list_items(document),
          "a list item under a list group is where it belongs");
  const std::string before = document.SerializeAsString();
  render::migrate_misplaced_list_items(&document);
  require_equal(document.SerializeAsString(), before,
                "the migration leaves a well formed list alone");
}

void verify_a_parentless_list_item_is_left_where_it_is() {
  docv1::Document document = base_document("list.md");
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "orphan");
  base_of(&document, 0)->clear_parent();

  require(!render::has_misplaced_list_items(document),
          "a parentless list item has no parent to be misplaced under");
  const std::string before = document.SerializeAsString();
  render::migrate_misplaced_list_items(&document);
  require_equal(document.SerializeAsString(), before,
                "a parentless list item cannot be re-homed, so nothing moves");
}

void verify_a_list_item_under_the_body_gets_a_synthesized_group() {
  docv1::Document document = base_document("stray.md");
  add_paragraph(&document, "#/body", "before");
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "alpha");

  require(render::has_misplaced_list_items(document), "a list item under the body is misplaced");
  render::migrate_misplaced_list_items(&document);

  require_equal(document.groups_size(), 1, "the migration synthesizes exactly one list group");
  require_equal(static_cast<int>(document.groups(0).label()),
                static_cast<int>(docv1::GROUP_LABEL_LIST),
                "the synthesized group is a list group");
  require_equal(document.groups(0).parent().ref(), "#/body",
                "the synthesized group takes the item's old parent");
  require_equal(document.groups(0).name(), "group",
                "the synthesized group carries the model's name");
  require_equal(children_of(document.body().children()), "#/texts/0, #/groups/0",
                "the group stands where the stray item stood");
  require_equal(children_of(document.groups(0).children()), "#/texts/1",
                "the salvaged item hangs under the new group");
  require_equal(document.texts_size(), 2, "the item is re-appended, not duplicated");
  require_equal(const_base_of(document, 1).text(), "alpha", "the item keeps its text");
  require_equal(const_base_of(document, 1).parent().ref(), "#/groups/0",
                "the re-appended item names the new group as its parent");
  require(!render::has_misplaced_list_items(document), "the migration is a fixed point");
}

void verify_a_run_of_stray_items_shares_one_group() {
  docv1::Document document = base_document("stray.md");
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "alpha");
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "beta");
  add_paragraph(&document, "#/body", "after");

  render::migrate_misplaced_list_items(&document);

  require_equal(document.groups_size(), 1, "one uninterrupted run gets one group");
  require_equal(children_of(document.body().children()), "#/groups/0, #/texts/0",
                "the group replaces the run and the paragraph renumbers down");
  require_equal(const_base_of(document, 0).text(), "after",
                "the surviving paragraph is the one the reference now names");
  require_equal(children_of(document.groups(0).children()), "#/texts/1, #/texts/2",
                "the run's items keep their order under the group");
  require_equal(const_base_of(document, 1).text(), "alpha", "the first item comes first");
  require_equal(const_base_of(document, 2).text(), "beta", "the second item comes second");
}

void verify_a_paragraph_between_stray_items_breaks_the_run() {
  docv1::Document document = base_document("stray.md");
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "alpha");
  add_paragraph(&document, "#/body", "interrupting");
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "beta");

  render::migrate_misplaced_list_items(&document);

  require_equal(document.groups_size(), 2, "a broken run gets one group per stretch");
  // Later runs are re-homed first, so the trailing run's group is groups/0.
  require_equal(children_of(document.body().children()), "#/groups/1, #/texts/0, #/groups/0",
                "each group stands where its run stood");
  require_equal(children_of(document.groups(1).children()), "#/texts/2",
                "the group synthesized for the leading run holds the leading item");
  require_equal(children_of(document.groups(0).children()), "#/texts/1",
                "the group synthesized for the trailing run holds the trailing item");
  require_equal(const_base_of(document, 2).text(), "alpha", "the leading item keeps its text");
  require_equal(const_base_of(document, 1).text(), "beta", "the trailing item keeps its text");
  require_equal(const_base_of(document, 0).text(), "interrupting",
                "the paragraph that broke the run stays in the body");
}

void verify_the_salvaged_item_keeps_its_marker_and_numbering() {
  docv1::Document document = base_document("stray.md");
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "one", 0, true);
  document.mutable_texts(0)->mutable_list_item()->set_marker("1.");
  base_of(&document, 0)->set_hyperlink("https://example.test/one");

  render::migrate_misplaced_list_items(&document);

  const docv1::ListItem& moved = document.texts(0).list_item();
  require(moved.enumerated(), "an enumerated item stays enumerated");
  require_equal(moved.marker(), "1.", "the item's own marker survives the move");
  require_equal(moved.base().hyperlink(), "https://example.test/one",
                "the item's hyperlink survives the move");
  require_equal(moved.base().orig(), "one",
                "an item with no original text takes its text as the original");
}

void verify_a_plain_text_item_labelled_as_a_list_item_migrates_too() {
  docv1::Document document = base_document("stray.md");
  add_text(&document, "#/body", docv1::BaseTextItem::kText, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "labelled only");

  require(render::has_misplaced_list_items(document),
          "the list-item label alone makes an entry a list item");
  render::migrate_misplaced_list_items(&document);
  require_equal(document.texts_size(), 1, "the entry is replaced, not duplicated");
  require_equal(static_cast<int>(document.texts(0).item_case()),
                static_cast<int>(docv1::BaseTextItem::kListItem),
                "the re-appended entry is a proper list item");
  require_equal(document.texts(0).list_item().marker(), "-",
                "an item with no marker of its own takes the model's default");
}

void verify_only_the_first_provenance_entry_rides_along() {
  docv1::Document document = base_document("stray.md");
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "alpha");
  auto* prov = base_of(&document, 0)->mutable_prov();
  add_prov(prov, 1, 10, 20, 30, 40);
  add_prov(prov, 2, 50, 60, 70, 80);

  render::migrate_misplaced_list_items(&document);

  const docv1::TextItemBase& moved = document.texts(0).list_item().base();
  require_equal(moved.prov_size(), 1, "the model's re-add carries only the first provenance entry");
  require_equal(moved.prov(0).page_no(), 1, "the entry it carries is the first one");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("load-normalization-test", "ok", {
      verify_a_document_inside_its_pages_needs_no_clamping,
      verify_boxes_outside_their_page_are_pulled_back,
      verify_pictures_and_tables_clamp_too,
      verify_a_page_the_document_never_declares_is_left_alone,
      verify_table_cells_clamp_only_when_the_table_sits_on_one_page,
      verify_ordered_list_groups_are_relabelled_to_plain_lists,
      verify_the_roots_are_relabelled_as_well,
      verify_relabelling_must_run_before_the_migration,
      verify_a_list_item_in_a_list_group_stays_put,
      verify_a_parentless_list_item_is_left_where_it_is,
      verify_a_list_item_under_the_body_gets_a_synthesized_group,
      verify_a_run_of_stray_items_shares_one_group,
      verify_a_paragraph_between_stray_items_breaks_the_run,
      verify_the_salvaged_item_keeps_its_marker_and_numbering,
      verify_a_plain_text_item_labelled_as_a_list_item_migrates_too,
      verify_only_the_first_provenance_entry_rides_along,
  });
}
