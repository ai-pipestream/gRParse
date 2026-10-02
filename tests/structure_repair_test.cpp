// Proves the opt-in structural repairs (docling-core #810) on hand-built
// Documents: the furniture tree migrated into the body page by page with
// the furniture layer, orphaned captions listed by the item that names
// them, list children wrapped in new list items, empty groups removed with
// the arena renumbered; each one idempotent, each one leaving the rule it
// repairs satisfied, the counts reaching RepairReport and the process
// totals, and the defaults leaving all four off. Also pins the renumbering
// that retire_text_items owes items outside the tree (docling-core #810's
// delete_items fix): orphans, graph cells, a non-group parent, and the
// retired item's own children.

#include <optional>
#include <string>
#include <vector>

#include <google/protobuf/util/message_differencer.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/docling_map.h"
#include "grparse/document_repair.h"
#include "grparse/structure_rules.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

using grparse::StructureRule;
using grparse_test::require;
using grparse_test::require_equal;
using google::protobuf::util::MessageDifferencer;

docv1::Document base_document(int pages = 0) {
  docv1::Document document;
  document.set_name("structure.pdf");
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

docv1::GroupItem* group_at(docv1::Document* document, const std::string& ref) {
  return document->mutable_groups(std::stoi(ref.substr(std::string("#/groups/").size())));
}

void list_under(docv1::Document* document, const std::string& parent, const std::string& ref) {
  if (parent == "#/body") {
    document->mutable_body()->add_children()->set_ref(ref);
  } else if (parent == "#/furniture") {
    document->mutable_furniture()->add_children()->set_ref(ref);
  } else if (parent.starts_with("#/groups/")) {
    group_at(document, parent)->add_children()->set_ref(ref);
  }
}

// A text item, listed by `parent` when `listed`, on `page` between `top`
// and `bottom` (top-left origin) when `page` is non-zero.
std::string add_text(docv1::Document* document, const std::string& parent, const std::string& text,
                     docv1::DocItemLabel label = docv1::DOC_ITEM_LABEL_TEXT, int page = 0,
                     double top = 0, double bottom = 0, bool listed = true) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* base = document->add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref(parent);
  base->set_label(label);
  base->set_content_layer(parent == "#/furniture" ? docv1::CONTENT_LAYER_FURNITURE
                                                  : docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  base->set_orig(text);
  if (page > 0) {
    auto* prov = base->add_prov();
    prov->set_page_no(page);
    auto* box = prov->mutable_bbox();
    box->set_l(60);
    box->set_r(740);
    box->set_t(top);
    box->set_b(bottom);
    box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  }
  if (listed) list_under(document, parent, ref);
  return ref;
}

std::string add_list_item(docv1::Document* document, const std::string& parent, bool enumerated) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* item = document->add_texts()->mutable_list_item();
  item->set_enumerated(enumerated);
  item->set_marker(enumerated ? "1." : "-");
  auto* base = item->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref(parent);
  base->set_label(docv1::DOC_ITEM_LABEL_LIST_ITEM);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text("item " + ref);
  list_under(document, parent, ref);
  return ref;
}

std::string add_group(docv1::Document* document, docv1::GroupLabel label,
                      const std::string& parent = "#/body", bool listed = true) {
  const std::string ref = "#/groups/" + std::to_string(document->groups_size());
  auto* group = document->add_groups();
  group->set_self_ref(ref);
  group->mutable_parent()->set_ref(parent);
  group->set_label(label);
  group->set_content_layer(parent == "#/furniture" ? docv1::CONTENT_LAYER_FURNITURE
                                                   : docv1::CONTENT_LAYER_BODY);
  if (listed) list_under(document, parent, ref);
  return ref;
}

const docv1::TextItemBase& text_base(const docv1::Document& document, const std::string& ref) {
  const auto& item = document.texts(std::stoi(ref.substr(std::string("#/texts/").size())));
  return item.has_list_item() ? item.list_item().base() : item.text().base();
}

std::vector<std::string> refs(const google::protobuf::RepeatedPtrField<docv1::RefItem>& list) {
  std::vector<std::string> out;
  for (const auto& entry : list) out.push_back(entry.ref());
  return out;
}

std::string joined(const std::vector<std::string>& parts) {
  std::string out;
  for (const auto& part : parts) out += "[" + part + "]";
  return out;
}

bool breaks(const docv1::Document& document, StructureRule rule) {
  return !grparse::docling_structure_findings(document, {rule}).empty();
}

// Runs `repair` a second time and requires it to find nothing and change
// nothing.
template <typename Repair>
void require_idempotent(docv1::Document* document, Repair repair, const std::string& what) {
  const docv1::Document settled = *document;
  require_equal(repair(document), 0, what + ": a second run found work");
  require(MessageDifferencer::Equals(settled, *document), what + ": a second run changed it");
}

// ---------------------------------------------------------------------------

void verify_defaults_leave_structure_alone() {
  const grparse::RepairOptions options;
  require(!options.migrate_furniture_tree && !options.repair_referenced_orphans &&
              !options.wrap_list_children && !options.remove_empty_groups,
          "the four structural repairs are opt-in");
  docv1::Document document = base_document(1);
  add_text(&document, "#/body", "Body text", docv1::DOC_ITEM_LABEL_TEXT, 1, 400, 420);
  add_text(&document, "#/furniture", "Header", docv1::DOC_ITEM_LABEL_PAGE_HEADER, 1, 10, 30);
  add_group(&document, docv1::GROUP_LABEL_SECTION);
  const grparse::RepairReport report = grparse::repair_document(&document, options);
  require(report.furniture_tree_migrated == 0 && report.empty_groups_removed == 0,
          "the default pass reports no structural repair");
  require_equal(document.furniture().children_size(), 1, "the furniture tree is kept");
  require_equal(document.groups_size(), 1, "the empty group is kept");
}

void verify_furniture_migration_places_by_page() {
  docv1::Document document = base_document(2);
  const std::string b1 = add_text(&document, "#/body", "Page one body", docv1::DOC_ITEM_LABEL_TEXT,
                                  1, 400, 440);
  const std::string b2 = add_text(&document, "#/body", "Page two body", docv1::DOC_ITEM_LABEL_TEXT,
                                  2, 400, 440);
  const std::string f2 = add_text(&document, "#/furniture", "2", docv1::DOC_ITEM_LABEL_PAGE_FOOTER,
                                  2, 950, 980);
  const std::string h1 = add_text(&document, "#/furniture", "Report",
                                  docv1::DOC_ITEM_LABEL_PAGE_HEADER, 1, 10, 30);
  // Unlabelled furniture is classified by where its box sits.
  const std::string f1 = add_text(&document, "#/furniture", "Confidential",
                                  docv1::DOC_ITEM_LABEL_TEXT, 1, 940, 960);
  const std::string h2 = add_text(&document, "#/furniture", "Report",
                                  docv1::DOC_ITEM_LABEL_PAGE_HEADER, 2, 10, 30);
  // A furniture group with no page leads the body; its body-layer child
  // takes the furniture layer with it.
  const std::string notes = add_group(&document, docv1::GROUP_LABEL_SECTION, "#/furniture");
  const std::string note = add_text(&document, notes, "Unplaced note");
  require(breaks(document, StructureRule::kDeprecatedFurnitureTree), "the fixture uses the tree");

  require_equal(grparse::migrate_furniture_tree(&document), 5, "five furniture children move");
  const std::vector<std::string> expected = {notes, h1, b1, f1, h2, b2, f2};
  require(refs(document.body().children()) == expected,
          "headers lead their page and footers close it; got " +
              joined(refs(document.body().children())));
  require_equal(document.furniture().children_size(), 0, "the furniture tree is empty");
  for (const std::string& ref : {h1, f1, h2, f2}) {
    const auto& base = text_base(document, ref);
    require_equal(base.parent().ref(), std::string("#/body"), ref + " is a body child");
    require(base.content_layer() == docv1::CONTENT_LAYER_FURNITURE, ref + " stays furniture");
  }
  require(document.groups(0).content_layer() == docv1::CONTENT_LAYER_FURNITURE &&
              text_base(document, note).content_layer() == docv1::CONTENT_LAYER_FURNITURE,
          "a migrated group's body-layer descendants take the furniture layer");
  require(text_base(document, b1).content_layer() == docv1::CONTENT_LAYER_BODY,
          "body items keep the body layer");
  require(!breaks(document, StructureRule::kDeprecatedFurnitureTree), "the rule now holds");
  require(grparse::docling_integrity_errors(document).empty(), "the document stays well linked");
  require_idempotent(&document, grparse::migrate_furniture_tree, "furniture migration");
}

// With no page on any body item, the docling-core placement applies: every
// header before the body, every footer after it.
void verify_furniture_migration_without_body_pages() {
  docv1::Document document = base_document(2);
  const std::string body = add_text(&document, "#/body", "Unplaced body");
  const std::string f1 = add_text(&document, "#/furniture", "1", docv1::DOC_ITEM_LABEL_PAGE_FOOTER,
                                  1, 950, 980);
  const std::string h2 = add_text(&document, "#/furniture", "Report",
                                  docv1::DOC_ITEM_LABEL_PAGE_HEADER, 2, 10, 30);
  const std::string h1 = add_text(&document, "#/furniture", "Report",
                                  docv1::DOC_ITEM_LABEL_PAGE_HEADER, 1, 10, 30);
  require_equal(grparse::migrate_furniture_tree(&document), 3, "three furniture children move");
  const std::vector<std::string> expected = {h1, h2, body, f1};
  require(refs(document.body().children()) == expected,
          "headers before the body, footers after; got " +
              joined(refs(document.body().children())));
}

void verify_referenced_orphans_are_listed() {
  docv1::Document document = base_document();
  auto* table = document.add_tables();
  table->set_self_ref("#/tables/0");
  table->mutable_parent()->set_ref("#/body");
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  document.mutable_body()->add_children()->set_ref("#/tables/0");
  const std::string caption = add_text(&document, "#/tables/0", "Table 1. Results",
                                       docv1::DOC_ITEM_LABEL_CAPTION, 0, 0, 0, false);
  const std::string footnote = add_text(&document, "#/tables/0", "a. rounded",
                                        docv1::DOC_ITEM_LABEL_FOOTNOTE, 0, 0, 0, false);
  // Named by the table but parented elsewhere: not this repair's to move.
  const std::string elsewhere = add_text(&document, "#/body", "See also");
  document.mutable_tables(0)->add_captions()->set_ref(caption);
  document.mutable_tables(0)->add_footnotes()->set_ref(footnote);
  document.mutable_tables(0)->add_references()->set_ref(elsewhere);
  require(breaks(document, StructureRule::kNotListedByParent), "the fixture has orphans");

  require_equal(grparse::repair_referenced_orphans(&document), 2, "caption and footnote repaired");
  const std::vector<std::string> expected = {caption, footnote};
  require(refs(document.tables(0).children()) == expected, "the table lists both");
  require(!breaks(document, StructureRule::kNotListedByParent), "no orphan is left");
  require_idempotent(&document, grparse::repair_referenced_orphans, "orphan repair");
}

void verify_list_children_are_wrapped() {
  docv1::Document document = base_document();
  const std::string list = add_group(&document, docv1::GROUP_LABEL_LIST);
  const std::string first = add_list_item(&document, list, true);
  const std::string stray = add_text(&document, list, "a paragraph inside the list");
  const std::string nested = add_group(&document, docv1::GROUP_LABEL_LIST, list);
  add_list_item(&document, nested, false);
  const std::string last = add_list_item(&document, list, true);
  // A child the list lists but that names another parent is skipped.
  const std::string foreign = add_text(&document, "#/body", "listed twice");
  group_at(&document, list)->add_children()->set_ref(foreign);
  require(breaks(document, StructureRule::kListGroupNonListItemChild), "the fixture breaks it");

  const int texts_before = document.texts_size();
  require_equal(grparse::wrap_list_children(&document), 2, "the paragraph and the sublist wrap");
  const std::string wrap_stray = "#/texts/" + std::to_string(texts_before);
  const std::string wrap_nested = "#/texts/" + std::to_string(texts_before + 1);
  const std::vector<std::string> expected = {first, wrap_stray, wrap_nested, last, foreign};
  require(refs(group_at(&document, list)->children()) == expected,
          "each wrapper takes its child's position; got " +
              joined(refs(group_at(&document, list)->children())));
  const auto& item = document.texts(texts_before).list_item();
  require(item.enumerated(), "the wrapper is enumerated like its siblings");
  require(item.base().text().empty() && item.base().parent().ref() == list &&
              refs(item.base().children()) == std::vector<std::string>{stray},
          "the wrapper is an empty list item holding the stray child");
  require_equal(text_base(document, stray).parent().ref(), wrap_stray, "the child is re-parented");
  require_equal(group_at(&document, nested)->parent().ref(), wrap_nested,
                "the sublist hangs under its new item");
  require(grparse::docling_structure_findings(document, {StructureRule::kListGroupNonListItemChild})
                  .size() == 1,
          "only the foreign child, which names another parent, is left");
  require_idempotent(&document, grparse::wrap_list_children, "list wrapping");

  // With no list item sibling, an ordered list's wrapper is enumerated.
  docv1::Document ordered = base_document();
  const std::string numbers = add_group(&ordered, docv1::GROUP_LABEL_ORDERED_LIST);
  add_text(&ordered, numbers, "lonely paragraph");
  require_equal(grparse::wrap_list_children(&ordered), 1, "the paragraph wraps");
  require(ordered.texts(1).list_item().enumerated(), "an ordered list numbers its wrapper");
}

void verify_empty_groups_are_removed() {
  docv1::Document document = base_document();
  add_group(&document, docv1::GROUP_LABEL_SECTION);                                   // groups/0
  const std::string outer = add_group(&document, docv1::GROUP_LABEL_CHAPTER);         // groups/1
  const std::string inner = add_group(&document, docv1::GROUP_LABEL_SECTION, outer);  // groups/2
  const std::string kept = add_group(&document, docv1::GROUP_LABEL_SECTION);          // groups/3
  const std::string text = add_text(&document, kept, "kept text");
  // An anchor into a removed group follows it to its parent.
  auto* anchor = document.add_anchors();
  anchor->set_name("inner");
  anchor->mutable_target()->set_ref(inner);
  require(breaks(document, StructureRule::kEmptyGroup), "the fixture has empty groups");

  require_equal(grparse::remove_empty_groups(&document), 3,
                "the empty group, then the inner one, then the outer one it emptied");
  require_equal(document.groups_size(), 1, "one group is left");
  require_equal(document.groups(0).self_ref(), std::string("#/groups/0"), "renumbered");
  require(refs(document.body().children()) == std::vector<std::string>{"#/groups/0"},
          "the body lists the survivor under its new name");
  require_equal(text_base(document, text).parent().ref(), std::string("#/groups/0"),
                "the survivor's child follows the rename");
  require_equal(document.anchors(0).target().ref(), std::string("#/body"),
                "an anchor into the removed chain lands on the body, not on another group");
  require(grparse::docling_integrity_errors(document).empty(), "the document stays well linked");
  require(!breaks(document, StructureRule::kEmptyGroup), "no empty group is left");
  require_idempotent(&document, grparse::remove_empty_groups, "empty group removal");

  // A group an orphan claims as its parent is kept, as in docling-core.
  docv1::Document claimed = base_document();
  const std::string holder = add_group(&claimed, docv1::GROUP_LABEL_SECTION);
  add_text(&claimed, holder, "orphan", docv1::DOC_ITEM_LABEL_TEXT, 0, 0, 0, false);
  require_equal(grparse::remove_empty_groups(&claimed), 0, "a claimed group stays");
}

// The pass runs them when asked, counts them, adds them to the totals, and
// a second pass is a no-op.
void verify_the_pass_runs_and_counts_them() {
  docv1::Document document = base_document(1);
  add_text(&document, "#/body", "Body", docv1::DOC_ITEM_LABEL_TEXT, 1, 400, 420);
  add_text(&document, "#/furniture", "Header", docv1::DOC_ITEM_LABEL_PAGE_HEADER, 1, 10, 30);
  const std::string list = add_group(&document, docv1::GROUP_LABEL_LIST);
  add_text(&document, list, "stray");
  add_group(&document, docv1::GROUP_LABEL_SECTION);
  auto* picture = document.add_pictures();
  picture->set_self_ref("#/pictures/0");
  picture->mutable_parent()->set_ref("#/body");
  document.mutable_body()->add_children()->set_ref("#/pictures/0");
  const std::string caption = add_text(&document, "#/pictures/0", "Figure 1",
                                       docv1::DOC_ITEM_LABEL_CAPTION, 0, 0, 0, false);
  document.mutable_pictures(0)->add_captions()->set_ref(caption);

  grparse::RepairOptions options;
  options.migrate_furniture_tree = true;
  options.repair_referenced_orphans = true;
  options.wrap_list_children = true;
  options.remove_empty_groups = true;
  const grparse::RepairTotals before = grparse::repair_totals();
  const grparse::RepairReport report = grparse::run_repair_pass(&document, options);
  const grparse::RepairTotals after = grparse::repair_totals();
  require(report.furniture_tree_migrated == 1 && report.orphans_repaired == 1 &&
              report.list_children_wrapped == 1 && report.empty_groups_removed == 1,
          "each structural repair counted once");
  require(report.changed_anything() && report.changed_text_or_arenas(),
          "wrapping and removal change the arenas");
  require(after.furniture_tree_migrated - before.furniture_tree_migrated == 1 &&
              after.orphans_repaired - before.orphans_repaired == 1 &&
              after.list_children_wrapped - before.list_children_wrapped == 1 &&
              after.empty_groups_removed - before.empty_groups_removed == 1,
          "the totals carry the counts");
  require(grparse::docling_structure_findings(document).empty(),
          "the repaired document follows every rule; got " +
              std::to_string(grparse::docling_structure_findings(document).size()) + " finding(s)");
  const docv1::Document settled = document;
  require(!grparse::repair_document(&document, options).changed_anything(),
          "a second pass found work");
  require(MessageDifferencer::Equals(settled, document), "a second pass changed the document");
}

// docling-core #810 fixed delete_items to renumber every node, not only
// those reachable from the body, and graph cells' item_refs. Here the
// retirement rewrites the whole Document, so an orphan and a graph cell are
// renumbered; a cell into the retired item follows it to its survivor
// (docling-core clears it, but the content is not gone, it was absorbed);
// the retired item's parent (here a picture, not a group) drops it instead
// of listing the survivor as a second child; and the retired item's
// children move to the survivor.
void verify_retirement_renumbers_outside_the_tree() {
  docv1::Document document = base_document();
  const std::string head = add_text(&document, "#/body", "head");  // texts/0
  // The retired item is a picture's child (a run of text inside a figure).
  const std::string tail = add_text(&document, "#/pictures/0", "tail", docv1::DOC_ITEM_LABEL_TEXT,
                                    0, 0, 0, false);  // texts/1, retired
  const std::string kept = add_text(&document, "#/body", "kept");  // texts/2 -> 1
  const std::string orphan = add_text(&document, "#/body", "orphan", docv1::DOC_ITEM_LABEL_TEXT,
                                      0, 0, 0, false);  // texts/3 -> 2
  const std::string nested = add_text(&document, tail, "nested", docv1::DOC_ITEM_LABEL_TEXT, 0, 0,
                                      0, false);  // texts/4 -> 3
  document.mutable_texts(1)->mutable_text()->mutable_base()->add_children()->set_ref(nested);
  auto* picture = document.add_pictures();
  picture->set_self_ref("#/pictures/0");
  picture->mutable_parent()->set_ref("#/body");
  picture->add_children()->set_ref(tail);
  document.mutable_body()->add_children()->set_ref("#/pictures/0");
  auto* form = document.add_form_items();
  form->set_self_ref("#/form_items/0");
  form->mutable_parent()->set_ref("#/body");
  document.mutable_body()->add_children()->set_ref("#/form_items/0");
  auto* to_orphan = form->mutable_graph()->add_cells();
  to_orphan->set_cell_id(0);
  to_orphan->mutable_item_ref()->set_ref(orphan);
  auto* to_tail = form->mutable_graph()->add_cells();
  to_tail->set_cell_id(1);
  to_tail->mutable_item_ref()->set_ref(tail);
  auto* to_kept = form->mutable_graph()->add_cells();
  to_kept->set_cell_id(2);
  to_kept->mutable_item_ref()->set_ref(kept);

  grparse::retire_text_items(&document, {{tail, head}});

  require_equal(document.texts_size(), 4, "one text retired");
  require_equal(text_base(document, "#/texts/2").text(), std::string("orphan"),
                "the orphan moved down a slot");
  require_equal(text_base(document, "#/texts/2").self_ref(), std::string("#/texts/2"),
                "and its self_ref was renumbered although no parent lists it");
  const auto& cells = document.form_items(0).graph().cells();
  require_equal(cells[0].item_ref().ref(), std::string("#/texts/2"), "a cell into the orphan");
  require_equal(cells[1].item_ref().ref(), head, "a cell into the retired item follows it");
  require_equal(cells[2].item_ref().ref(), std::string("#/texts/1"), "a cell into a kept item");
  require(document.pictures(0).children_size() == 0,
          "the picture, the retired item's parent, no longer lists it, nor its survivor");
  require(refs(text_base(document, head).children()) == std::vector<std::string>{"#/texts/3"},
          "the survivor adopts the retired item's child");
  require_equal(text_base(document, "#/texts/3").parent().ref(), head,
                "and the child names the survivor");
  require(grparse::docling_structure_findings(
              document, {StructureRule::kNotListedByParent, StructureRule::kParentMissing})
                  .size() == 1,
          "only the deliberate orphan is left unlisted");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("structure-repair-test", "ok", {
      verify_defaults_leave_structure_alone,
      verify_furniture_migration_places_by_page,
      verify_furniture_migration_without_body_pages,
      verify_referenced_orphans_are_listed,
      verify_list_children_are_wrapped,
      verify_empty_groups_are_removed,
      verify_the_pass_runs_and_counts_them,
      verify_retirement_renumbers_outside_the_tree,
  });
}
