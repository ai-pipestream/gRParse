// The load-time normalizations, shared by every export renderer; contract on
// the declarations in load_normalization.h.
#include "load_normalization.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace grparse::render {

namespace docv1 = ai::pipestream::document::v1;

// The implementation keeps internal linkage (anonymous namespace) while
// staying addressable under a name, so the exported wrappers below can reach
// it without publishing the two dozen helpers it is built from.
namespace load_norm {
namespace {


// ---------------------------------------------------------------------------
// Load normalization. The reference model applies two mutations while
// loading a document, before any dump: provenance bounding boxes are clamped
// to their page, and list items whose parent is not a list group are moved
// into a synthesized list group (re-appended at the end of the text arena,
// with every reference renumbered). Both are reproduced here on a private
// copy so the rendered output matches the reference dump exactly.
// ---------------------------------------------------------------------------

double clamp_coordinate(double value, double hi) {
  return std::min(std::max(value, 0.0), std::max(hi, 0.0));
}

void clamp_bbox(docv1::BoundingBox* bbox, const docv1::Size& page) {
  bbox->set_l(clamp_coordinate(bbox->l(), page.width()));
  bbox->set_r(clamp_coordinate(bbox->r(), page.width()));
  bbox->set_t(clamp_coordinate(bbox->t(), page.height()));
  bbox->set_b(clamp_coordinate(bbox->b(), page.height()));
}

void clamp_prov_list(docv1::Document* doc,
                     google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* provs) {
  for (auto& prov : *provs) {
    const auto page = doc->pages().find(prov.page_no());
    if (page != doc->pages().end()) clamp_bbox(prov.mutable_bbox(), page->second.size());
  }
}

template <typename Message>
void clamp_graph_item(docv1::Document* doc, Message* item) {
  clamp_prov_list(doc, item->mutable_prov());
  for (auto& cell : *item->mutable_graph()->mutable_cells()) {
    if (!cell.has_prov()) continue;
    const auto page = doc->pages().find(cell.prov().page_no());
    if (page != doc->pages().end()) {
      clamp_bbox(cell.mutable_prov()->mutable_bbox(), page->second.size());
    }
  }
}

void clamp_document(docv1::Document* doc) {
  for (auto& text : *doc->mutable_texts()) {
    switch (text.item_case()) {
      case docv1::BaseTextItem::kTitle:
        clamp_prov_list(doc, text.mutable_title()->mutable_base()->mutable_prov());
        break;
      case docv1::BaseTextItem::kSectionHeader:
        clamp_prov_list(doc, text.mutable_section_header()->mutable_base()->mutable_prov());
        break;
      case docv1::BaseTextItem::kFieldHeading:
        clamp_prov_list(doc, text.mutable_field_heading()->mutable_base()->mutable_prov());
        break;
      case docv1::BaseTextItem::kFieldValue:
        clamp_prov_list(doc, text.mutable_field_value()->mutable_base()->mutable_prov());
        break;
      case docv1::BaseTextItem::kListItem:
        clamp_prov_list(doc, text.mutable_list_item()->mutable_base()->mutable_prov());
        break;
      case docv1::BaseTextItem::kCode:
        clamp_prov_list(doc, text.mutable_code()->mutable_prov());
        break;
      case docv1::BaseTextItem::kFormula:
        clamp_prov_list(doc, text.mutable_formula()->mutable_base()->mutable_prov());
        break;
      case docv1::BaseTextItem::kText:
        clamp_prov_list(doc, text.mutable_text()->mutable_base()->mutable_prov());
        break;
      default:
        break;
    }
  }
  for (auto& picture : *doc->mutable_pictures()) {
    clamp_prov_list(doc, picture.mutable_prov());
  }
  for (auto& table : *doc->mutable_tables()) {
    clamp_prov_list(doc, table.mutable_prov());
    // Cell boxes clamp only when every table provenance names one page and
    // that page is known.
    std::optional<std::int32_t> page_no;
    bool single_page = !table.prov().empty();
    for (const auto& prov : table.prov()) {
      if (page_no && *page_no != prov.page_no()) single_page = false;
      page_no = prov.page_no();
    }
    if (!single_page || !page_no) continue;
    const auto page = doc->pages().find(*page_no);
    if (page == doc->pages().end()) continue;
    for (auto& cell : *table.mutable_data()->mutable_table_cells()) {
      if (cell.has_bbox()) clamp_bbox(cell.mutable_bbox(), page->second.size());
    }
  }
  for (auto& item : *doc->mutable_key_value_items()) clamp_graph_item(doc, &item);
  for (auto& item : *doc->mutable_form_items()) clamp_graph_item(doc, &item);
  for (auto& item : *doc->mutable_field_regions()) clamp_prov_list(doc, item.mutable_prov());
  for (auto& item : *doc->mutable_field_items()) clamp_prov_list(doc, item.mutable_prov());
}

// A parsed "#/<arena>/<index>" reference; arena stays empty for roots and
// anything else that is not a three-part reference.
struct RefParts {
  std::string arena;
  int index = -1;
};

RefParts parse_ref_parts(std::string_view ref) {
  RefParts out;
  if (!ref.starts_with("#/")) return out;
  const std::size_t slash = ref.find('/', 2);
  if (slash == std::string_view::npos) return out;
  const std::string_view index_text = ref.substr(slash + 1);
  if (index_text.empty()) return out;
  int index = 0;
  for (const char c : index_text) {
    if (c < '0' || c > '9') return out;
    index = index * 10 + (c - '0');
  }
  out.arena = std::string(ref.substr(2, slash - 2));
  out.index = index;
  return out;
}

// Mutable views of the node fields the normalization touches. Null members
// mean the node type has no such field (or the reference did not resolve).
struct NodeFields {
  bool resolved = false;
  std::string* self_ref = nullptr;
  docv1::RefItem* parent = nullptr;  // null when unset or absent
  google::protobuf::RepeatedPtrField<docv1::RefItem>* children = nullptr;
  google::protobuf::RepeatedPtrField<docv1::FineRef>* comments = nullptr;
  google::protobuf::RepeatedPtrField<docv1::RefItem>* captions = nullptr;
  google::protobuf::RepeatedPtrField<docv1::RefItem>* references = nullptr;
  google::protobuf::RepeatedPtrField<docv1::RefItem>* footnotes = nullptr;
  docv1::TableData* table_data = nullptr;
};

template <typename Message>
NodeFields doc_item_fields(Message* item) {
  NodeFields out;
  out.resolved = true;
  out.self_ref = item->mutable_self_ref();
  if (item->has_parent()) out.parent = item->mutable_parent();
  out.children = item->mutable_children();
  out.comments = item->mutable_comments();
  return out;
}

template <typename Message>
NodeFields floating_item_fields(Message* item) {
  NodeFields out = doc_item_fields(item);
  out.captions = item->mutable_captions();
  out.references = item->mutable_references();
  out.footnotes = item->mutable_footnotes();
  return out;
}

NodeFields group_fields(docv1::GroupItem* group) {
  NodeFields out;
  out.resolved = true;
  out.self_ref = group->mutable_self_ref();
  if (group->has_parent()) out.parent = group->mutable_parent();
  out.children = group->mutable_children();
  return out;
}

const docv1::TextItemBase* const_text_base(const docv1::BaseTextItem& text) {
  switch (text.item_case()) {
    case docv1::BaseTextItem::kTitle: return &text.title().base();
    case docv1::BaseTextItem::kSectionHeader: return &text.section_header().base();
    case docv1::BaseTextItem::kFieldHeading: return &text.field_heading().base();
    case docv1::BaseTextItem::kFieldValue: return &text.field_value().base();
    case docv1::BaseTextItem::kListItem: return &text.list_item().base();
    case docv1::BaseTextItem::kFormula: return &text.formula().base();
    case docv1::BaseTextItem::kText: return &text.text().base();
    default: return nullptr;  // kCode inlines its base; unset has none
  }
}

docv1::TextItemBase* mutable_text_base(docv1::BaseTextItem* text) {
  switch (text->item_case()) {
    case docv1::BaseTextItem::kTitle: return text->mutable_title()->mutable_base();
    case docv1::BaseTextItem::kSectionHeader:
      return text->mutable_section_header()->mutable_base();
    case docv1::BaseTextItem::kFieldHeading:
      return text->mutable_field_heading()->mutable_base();
    case docv1::BaseTextItem::kFieldValue:
      return text->mutable_field_value()->mutable_base();
    case docv1::BaseTextItem::kListItem: return text->mutable_list_item()->mutable_base();
    case docv1::BaseTextItem::kFormula: return text->mutable_formula()->mutable_base();
    case docv1::BaseTextItem::kText: return text->mutable_text()->mutable_base();
    default: return nullptr;
  }
}

NodeFields node_fields(docv1::Document* doc, std::string_view ref) {
  if (ref == "#/body") return group_fields(doc->mutable_body());
  if (ref == "#/furniture") return group_fields(doc->mutable_furniture());
  const RefParts parts = parse_ref_parts(ref);
  const auto in_range = [&parts](int size) {
    return parts.index >= 0 && parts.index < size;
  };
  if (parts.arena == "groups" && in_range(doc->groups_size())) {
    return group_fields(doc->mutable_groups(parts.index));
  }
  if (parts.arena == "texts" && in_range(doc->texts_size())) {
    auto* text = doc->mutable_texts(parts.index);
    if (text->item_case() == docv1::BaseTextItem::kCode) {
      return floating_item_fields(text->mutable_code());
    }
    if (auto* base = mutable_text_base(text)) return doc_item_fields(base);
    return {};
  }
  if (parts.arena == "pictures" && in_range(doc->pictures_size())) {
    return floating_item_fields(doc->mutable_pictures(parts.index));
  }
  if (parts.arena == "tables" && in_range(doc->tables_size())) {
    NodeFields out = floating_item_fields(doc->mutable_tables(parts.index));
    out.table_data = doc->mutable_tables(parts.index)->mutable_data();
    return out;
  }
  if (parts.arena == "key_value_items" && in_range(doc->key_value_items_size())) {
    return floating_item_fields(doc->mutable_key_value_items(parts.index));
  }
  if (parts.arena == "form_items" && in_range(doc->form_items_size())) {
    return floating_item_fields(doc->mutable_form_items(parts.index));
  }
  if (parts.arena == "field_regions" && in_range(doc->field_regions_size())) {
    return doc_item_fields(doc->mutable_field_regions(parts.index));
  }
  if (parts.arena == "field_items" && in_range(doc->field_items_size())) {
    return doc_item_fields(doc->mutable_field_items(parts.index));
  }
  return {};
}

// Deleted indices per arena, in the model's renumbering formula: a
// three-part reference shifts down by the number of deleted indices at or
// below it.
using DeleteLookup = std::map<std::string, std::vector<int>, std::less<>>;

std::string renumbered_ref(const std::string& ref, const DeleteLookup& lookup) {
  const RefParts parts = parse_ref_parts(ref);
  if (parts.arena.empty()) return ref;
  const auto deleted = lookup.find(parts.arena);
  if (deleted == lookup.end()) return ref;
  int delta = 0;
  for (const int index : deleted->second) {
    if (parts.index >= index) --delta;
  }
  return "#/" + parts.arena + "/" + std::to_string(parts.index + delta);
}

// True when the text entry reconstructs as a list item: the dedicated arm,
// or the generic arm carrying the list-item label.
bool is_list_item_entry(const docv1::BaseTextItem& text) {
  if (text.item_case() == docv1::BaseTextItem::kListItem) return true;
  return text.item_case() == docv1::BaseTextItem::kText &&
         text.text().base().label() == docv1::DOC_ITEM_LABEL_LIST_ITEM;
}

bool is_list_group_ref(const docv1::Document* doc, std::string_view ref) {
  if (ref == "#/body") return doc->body().label() == docv1::GROUP_LABEL_LIST;
  if (ref == "#/furniture") return doc->furniture().label() == docv1::GROUP_LABEL_LIST;
  const RefParts parts = parse_ref_parts(ref);
  return parts.arena == "groups" && parts.index < doc->groups_size() &&
         doc->groups(parts.index).label() == docv1::GROUP_LABEL_LIST;
}

// ---------------------------------------------------------------------------
// Normalization detection. The render path copies the document only when one
// of the two load-time normalizations would actually change it; these
// predicates mirror clamp_document and the run collector CONSERVATIVELY. A
// false positive only costs the defensive copy; a false negative would skip
// a required normalization, so every approximation errs toward true. All
// reads are const, which keeps the zero-copy path safe for concurrent
// renders of one shared document (the emitter itself never mutates).
// ---------------------------------------------------------------------------

bool bbox_out_of_bounds(const docv1::BoundingBox& bbox, const docv1::Size& page) {
  return bbox.l() != clamp_coordinate(bbox.l(), page.width()) ||
         bbox.r() != clamp_coordinate(bbox.r(), page.width()) ||
         bbox.t() != clamp_coordinate(bbox.t(), page.height()) ||
         bbox.b() != clamp_coordinate(bbox.b(), page.height());
}

bool prov_list_needs_clamping(
    const docv1::Document& doc,
    const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& provs) {
  for (const auto& prov : provs) {
    const auto page = doc.pages().find(prov.page_no());
    if (page != doc.pages().end() &&
        bbox_out_of_bounds(prov.bbox(), page->second.size())) {
      return true;
    }
  }
  return false;
}

bool needs_clamping(const docv1::Document& doc) {
  for (const auto& text : doc.texts()) {
    const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* provs = nullptr;
    if (text.item_case() == docv1::BaseTextItem::kCode) {
      provs = &text.code().prov();
    } else if (const auto* base = const_text_base(text)) {
      provs = &base->prov();
    }
    if (provs != nullptr && prov_list_needs_clamping(doc, *provs)) return true;
  }
  for (const auto& picture : doc.pictures()) {
    if (prov_list_needs_clamping(doc, picture.prov())) return true;
  }
  for (const auto& table : doc.tables()) {
    if (prov_list_needs_clamping(doc, table.prov())) return true;
    std::optional<std::int32_t> page_no;
    bool single_page = !table.prov().empty();
    for (const auto& prov : table.prov()) {
      if (page_no && *page_no != prov.page_no()) single_page = false;
      page_no = prov.page_no();
    }
    if (!single_page || !page_no) continue;
    const auto page = doc.pages().find(*page_no);
    if (page == doc.pages().end()) continue;
    for (const auto& cell : table.data().table_cells()) {
      if (cell.has_bbox() && bbox_out_of_bounds(cell.bbox(), page->second.size())) {
        return true;
      }
    }
  }
  const auto graph_needs = [&doc](const auto& item) {
    if (prov_list_needs_clamping(doc, item.prov())) return true;
    for (const auto& cell : item.graph().cells()) {
      if (!cell.has_prov()) continue;
      const auto page = doc.pages().find(cell.prov().page_no());
      if (page != doc.pages().end() &&
          bbox_out_of_bounds(cell.prov().bbox(), page->second.size())) {
        return true;
      }
    }
    return false;
  };
  for (const auto& item : doc.key_value_items()) {
    if (graph_needs(item)) return true;
  }
  for (const auto& item : doc.form_items()) {
    if (graph_needs(item)) return true;
  }
  for (const auto& item : doc.field_regions()) {
    if (prov_list_needs_clamping(doc, item.prov())) return true;
  }
  for (const auto& item : doc.field_items()) {
    if (prov_list_needs_clamping(doc, item.prov())) return true;
  }
  return false;
}

// A linear over-approximation of the run collector: any list item whose
// parent is not a list group forces the migration pass. Items unreachable
// from the body would not actually migrate; treating them as if they would
// only costs the copy.
bool has_misplaced_list_items(const docv1::Document& doc) {
  for (const auto& text : doc.texts()) {
    if (!is_list_item_entry(text)) continue;
    const docv1::TextItemBase& base =
        text.item_case() == docv1::BaseTextItem::kListItem
            ? text.list_item().base()
            : text.text().base();
    if (base.has_parent() && !is_list_group_ref(&doc, base.parent().ref())) {
      return true;
    }
  }
  return false;
}

// Applies the delete renumbering to one node and recurses over its children
// (body tree only, exactly like the model: unreachable nodes keep stale
// references). Exact-match children/caption/reference/footnote entries to
// the deleted run refs are removed; comments and rich cell references are
// renumbered without removal.
void renumber_subtree(docv1::Document* doc, const std::string& ref,
                      const std::vector<std::string>& run_refs,
                      const DeleteLookup& lookup,
                      std::set<std::string>* visited) {
  if (!visited->insert(ref).second) return;
  NodeFields fields = node_fields(doc, ref);
  if (!fields.resolved) return;

  const auto is_run_ref = [&run_refs](const std::string& candidate) {
    return std::find(run_refs.begin(), run_refs.end(), candidate) != run_refs.end();
  };
  const auto filter_and_renumber =
      [&](google::protobuf::RepeatedPtrField<docv1::RefItem>* refs) {
        if (refs == nullptr) return;
        for (int i = refs->size() - 1; i >= 0; --i) {
          if (is_run_ref(refs->Get(i).ref())) {
            refs->DeleteSubrange(i, 1);
          } else {
            refs->Mutable(i)->set_ref(renumbered_ref(refs->Get(i).ref(), lookup));
          }
        }
      };

  if (fields.comments != nullptr) {
    for (auto& fine : *fields.comments) {
      fine.set_ref(renumbered_ref(fine.ref(), lookup));
    }
  }
  filter_and_renumber(fields.captions);
  filter_and_renumber(fields.references);
  filter_and_renumber(fields.footnotes);
  if (fields.table_data != nullptr) {
    for (auto& cell : *fields.table_data->mutable_table_cells()) {
      if (cell.has_ref()) {
        cell.mutable_ref()->set_ref(renumbered_ref(cell.ref().ref(), lookup));
      }
    }
  }
  if (fields.parent != nullptr) {
    fields.parent->set_ref(renumbered_ref(fields.parent->ref(), lookup));
  }
  *fields.self_ref = renumbered_ref(*fields.self_ref, lookup);
  filter_and_renumber(fields.children);

  const std::vector<std::string> child_refs = [&fields] {
    std::vector<std::string> out;
    for (const auto& child : *fields.children) out.push_back(child.ref());
    return out;
  }();
  for (const auto& child : child_refs) {
    renumber_subtree(doc, child, run_refs, lookup, visited);
  }
}

// Collects the subtree rooted at `ref` (the node itself plus every
// descendant reachable through children) into per-arena deletion sets.
void collect_subtree(docv1::Document* doc, const std::string& ref,
                     std::map<std::string, std::set<int>>* deleted,
                     std::set<std::string>* visited) {
  if (!visited->insert(ref).second) return;
  const RefParts parts = parse_ref_parts(ref);
  if (!parts.arena.empty()) (*deleted)[parts.arena].insert(parts.index);
  NodeFields fields = node_fields(doc, ref);
  if (!fields.resolved || fields.children == nullptr) return;
  const std::vector<std::string> child_refs = [&fields] {
    std::vector<std::string> out;
    for (const auto& child : *fields.children) out.push_back(child.ref());
    return out;
  }();
  for (const auto& child : child_refs) collect_subtree(doc, child, deleted, visited);
}

void delete_arena_entries(docv1::Document* doc, const std::string& arena,
                          const std::set<int>& indices) {
  const auto erase_descending = [&indices](auto* repeated) {
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
      if (*it >= 0 && *it < repeated->size()) repeated->DeleteSubrange(*it, 1);
    }
  };
  if (arena == "groups") erase_descending(doc->mutable_groups());
  else if (arena == "texts") erase_descending(doc->mutable_texts());
  else if (arena == "pictures") erase_descending(doc->mutable_pictures());
  else if (arena == "tables") erase_descending(doc->mutable_tables());
  else if (arena == "key_value_items") erase_descending(doc->mutable_key_value_items());
  else if (arena == "form_items") erase_descending(doc->mutable_form_items());
  else if (arena == "field_regions") erase_descending(doc->mutable_field_regions());
  else if (arena == "field_items") erase_descending(doc->mutable_field_items());
}

// Depth-first collection of misplaced list-item runs, mirroring the model's
// iteration: every node in the body tree in pre-order, runs continuing only
// while the previous visited node was a list item parented directly at the
// body (or parentless).
struct RunCollector {
  docv1::Document* doc = nullptr;
  std::vector<std::vector<int>> runs;
  bool prev_is_list_item = false;
  bool prev_parent_body_or_none = false;
  std::set<std::string> visited;

  void visit(const std::string& ref) {
    if (!visited.insert(ref).second) return;
    bool is_list_item = false;
    bool parent_body_or_none = false;
    const RefParts parts = parse_ref_parts(ref);
    if (parts.arena == "texts" && parts.index < doc->texts_size() &&
        is_list_item_entry(doc->texts(parts.index))) {
      is_list_item = true;
      const NodeFields fields = node_fields(doc, ref);
      const bool has_parent = fields.parent != nullptr;
      parent_body_or_none = !has_parent || fields.parent->ref() == "#/body";
      const bool misplaced =
          !has_parent || !is_list_group_ref(doc, fields.parent->ref());
      if (misplaced && has_parent) {  // a parentless item cannot be re-homed
        if (prev_is_list_item && prev_parent_body_or_none && !runs.empty()) {
          runs.back().push_back(parts.index);
        } else {
          runs.push_back({parts.index});
        }
      }
    }
    prev_is_list_item = is_list_item;
    prev_parent_body_or_none = parent_body_or_none;

    NodeFields fields = node_fields(doc, ref);
    if (!fields.resolved || fields.children == nullptr) return;
    const std::vector<std::string> child_refs = [&fields] {
      std::vector<std::string> out;
      for (const auto& child : *fields.children) out.push_back(child.ref());
      return out;
    }();
    for (const auto& child : child_refs) visit(child);
  }
};

// The list-item payload carried over into the synthesized group: everything
// the model's re-add copies (first provenance only), with metadata, sources,
// comments, and children left behind.
struct SalvagedListItem {
  docv1::TextItemBase base;
  bool enumerated = false;
  std::string marker = "-";
};

SalvagedListItem salvage_list_item(const docv1::BaseTextItem& text) {
  SalvagedListItem out;
  if (text.item_case() == docv1::BaseTextItem::kListItem) {
    out.base = text.list_item().base();
    out.enumerated = text.list_item().enumerated();
    if (text.list_item().has_marker()) out.marker = text.list_item().marker();
  } else {
    out.base = text.text().base();
  }
  return out;
}

void migrate_misplaced_list_items(docv1::Document* doc) {
  RunCollector collector;
  collector.doc = doc;
  collector.visit("#/body");
  if (collector.runs.empty()) return;

  // Later runs are re-homed first, exactly like the model.
  for (auto run = collector.runs.rbegin(); run != collector.runs.rend(); ++run) {
    const std::string first_ref = "#/texts/" + std::to_string((*run)[0]);
    const NodeFields first_fields = node_fields(doc, first_ref);
    if (!first_fields.resolved || first_fields.parent == nullptr) continue;
    const std::string parent_ref = first_fields.parent->ref();
    NodeFields parent_fields = node_fields(doc, parent_ref);
    if (!parent_fields.resolved || parent_fields.children == nullptr) continue;

    // Salvage the run's payloads before anything moves.
    std::vector<SalvagedListItem> salvaged;
    std::vector<std::string> run_refs;
    for (const int index : *run) {
      salvaged.push_back(salvage_list_item(doc->texts(index)));
      run_refs.push_back("#/texts/" + std::to_string(index));
    }

    // Insert the new list group before the run's first item.
    const std::string group_ref = "#/groups/" + std::to_string(doc->groups_size());
    auto* group = doc->add_groups();
    group->set_self_ref(group_ref);
    group->mutable_parent()->set_ref(parent_ref);
    group->set_content_layer(docv1::CONTENT_LAYER_BODY);
    group->set_name("group");
    group->set_label(docv1::GROUP_LABEL_LIST);
    parent_fields = node_fields(doc, parent_ref);  // pointers may have moved
    int position = parent_fields.children->size();
    for (int i = 0; i < parent_fields.children->size(); ++i) {
      if (parent_fields.children->Get(i).ref() == first_ref) {
        position = i;
        break;
      }
    }
    parent_fields.children->Add()->set_ref(group_ref);
    for (int i = parent_fields.children->size() - 1; i > position; --i) {
      parent_fields.children->SwapElements(i, i - 1);
    }

    // Delete the run items and their subtrees.
    std::map<std::string, std::set<int>> deleted;
    {
      std::set<std::string> visited;
      for (const auto& ref : run_refs) collect_subtree(doc, ref, &deleted, &visited);
    }
    for (const auto& [arena, indices] : deleted) {
      for (const int index : indices) {
        const std::string ref = "#/" + arena + "/" + std::to_string(index);
        const NodeFields fields = node_fields(doc, ref);
        if (!fields.resolved || fields.parent == nullptr) continue;
        NodeFields owner = node_fields(doc, fields.parent->ref());
        if (!owner.resolved || owner.children == nullptr) continue;
        for (int i = 0; i < owner.children->size(); ++i) {
          if (owner.children->Get(i).ref() == ref) {
            owner.children->DeleteSubrange(i, 1);
            break;
          }
        }
      }
    }
    DeleteLookup lookup;
    for (const auto& [arena, indices] : deleted) {
      lookup[arena].assign(indices.begin(), indices.end());
      delete_arena_entries(doc, arena, indices);
    }
    {
      std::set<std::string> visited;
      renumber_subtree(doc, "#/body", run_refs, lookup, &visited);
    }
    // Earlier (still pending) runs shift with the same renumbering.
    for (auto pending = std::next(run); pending != collector.runs.rend(); ++pending) {
      for (int& index : *pending) {
        const RefParts parts = parse_ref_parts(
            renumbered_ref("#/texts/" + std::to_string(index), lookup));
        index = parts.index;
      }
    }

    // Re-append the salvaged items under the group, in run order.
    const std::string final_group_ref =
        "#/groups/" + std::to_string(doc->groups_size() - 1);
    auto* final_group = doc->mutable_groups(doc->groups_size() - 1);
    for (const auto& item : salvaged) {
      const std::string new_ref = "#/texts/" + std::to_string(doc->texts_size());
      auto* entry = doc->add_texts()->mutable_list_item();
      auto* base = entry->mutable_base();
      base->set_self_ref(new_ref);
      base->mutable_parent()->set_ref(final_group_ref);
      base->set_content_layer(item.base.content_layer());
      base->set_label(docv1::DOC_ITEM_LABEL_LIST_ITEM);
      if (!item.base.prov().empty()) *base->add_prov() = item.base.prov(0);
      base->set_orig(!item.base.orig().empty() ? item.base.orig() : item.base.text());
      base->set_text(item.base.text());
      if (item.base.has_formatting()) *base->mutable_formatting() = item.base.formatting();
      if (item.base.has_hyperlink()) base->set_hyperlink(item.base.hyperlink());
      entry->set_enumerated(item.enumerated);
      entry->set_marker(item.marker);
      final_group->add_children()->set_ref(new_ref);
    }
  }
}

}  // namespace
}  // namespace load_norm

bool needs_clamping(const docv1::Document& doc) {
  return load_norm::needs_clamping(doc);
}

bool has_misplaced_list_items(const docv1::Document& doc) {
  return load_norm::has_misplaced_list_items(doc);
}

void clamp_document(docv1::Document* doc) { load_norm::clamp_document(doc); }

void migrate_misplaced_list_items(docv1::Document* doc) {
  load_norm::migrate_misplaced_list_items(doc);
}

}  // namespace grparse::render
