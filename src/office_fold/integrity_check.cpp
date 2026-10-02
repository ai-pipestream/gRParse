// The structural integrity walk over a mapped document. It is the fold's
// own contract check: every reference an item makes has to resolve, and
// every parent link has to be matched by the parent's children list.
#include "grparse/docling_map.h"
#include "grparse/structure_rules.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;

namespace {

using ChildRefs = google::protobuf::RepeatedPtrField<docv1::RefItem>;
using ProvenanceItems =
    google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>;

const docv1::TextItemBase* text_base(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle: return &item.title().base();
    case docv1::BaseTextItem::kSectionHeader:
      return &item.section_header().base();
    case docv1::BaseTextItem::kListItem: return &item.list_item().base();
    case docv1::BaseTextItem::kFormula: return &item.formula().base();
    case docv1::BaseTextItem::kText: return &item.text().base();
    case docv1::BaseTextItem::kFieldHeading:
      return &item.field_heading().base();
    case docv1::BaseTextItem::kFieldValue: return &item.field_value().base();
    case docv1::BaseTextItem::kCode:
    case docv1::BaseTextItem::ITEM_NOT_SET:
      return nullptr;
  }
  return nullptr;
}

// A page-plane locator needs a 1-based page. The page-less arms (a media
// time span, a byte range, a sheet cell, a line range) locate content in
// their own space and legitimately carry no page at all.
bool page_less(const docv1::ProvenanceItem& item) {
  return item.has_time() || item.has_byte_range() || item.has_grid()
      || item.has_line_range();
}

// One pass over every linked arena, gathering what the checks afterwards
// need: the references that exist, the parent links claimed, the children
// lists, and the graph cells that point into the item arenas.
class ReferenceWalk {
 public:
  explicit ReferenceWalk(const docv1::Document& document);

  // Every child reference resolves, and every parent lists the child that
  // claims it.
  void check_links();
  // The anchored references the fold resolves against the document-absolute
  // character space: a back-link, a change target, or a named anchor that
  // names nothing would be worse than one left unset.
  void check_anchored(const docv1::Document& document);

  std::vector<std::string> take_errors() { return std::move(errors_); }

  // A parent link check_links found broken: the parent resolves to nothing
  // (`missing`), or it does not list the child. The structural rules read
  // these typed instead of re-walking the arenas.
  struct ParentProblem {
    std::string child;
    std::string parent;
    bool missing = false;
  };
  const std::vector<ParentProblem>& parent_problems() const { return parent_problems_; }

 private:
  void collect(const std::string& self_ref, const ChildRefs& child_refs,
               bool has_parent, const std::string& parent_ref);
  // Provenance page numbers are 1-based in this dialect, so the proto3
  // default of 0 is never a page: an item carrying it points nowhere. The
  // box itself is not checked; zero-area placeholders are legitimate for
  // whole-sheet and whole-chart items that have no measured rectangle.
  void check_prov(const std::string& owner, const ProvenanceItems& prov);
  // The form arenas link into the item arenas through their graph cells as
  // well as through children, so a cell's item_ref is a reference like any
  // other and is resolved after the walk.
  void collect_graph(const std::string& owner, const docv1::GraphData& graph);

  void walk_roots(const docv1::Document& document);
  void walk_texts(const docv1::Document& document);
  // The four form arenas carry the same reference shape as the item arenas
  // above; merge and the renderers follow their links, so they are held to
  // the same contract.
  void walk_form_arenas(const docv1::Document& document);
  void check_resolves(const std::string& ref, const std::string& what);

  std::vector<std::string> errors_;
  std::set<std::string> refs_;
  // (item ref, parent ref) pairs and every children list, gathered in one
  // walk so parents can be validated against their children afterwards.
  std::vector<std::pair<std::string, std::string>> parents_;
  std::map<std::string, std::set<std::string>> children_;
  std::vector<std::pair<std::string, std::string>> graph_item_refs_;
  std::vector<ParentProblem> parent_problems_;
};

void ReferenceWalk::collect(const std::string& self_ref,
                            const ChildRefs& child_refs, bool has_parent,
                            const std::string& parent_ref) {
  if (self_ref.empty()) {
    errors_.push_back("item with empty self_ref");
    return;
  }
  if (!refs_.insert(self_ref).second) {
    errors_.push_back("duplicate self_ref " + self_ref);
  }
  for (const docv1::RefItem& child : child_refs) {
    children_[self_ref].insert(child.ref());
  }
  if (has_parent) parents_.emplace_back(self_ref, parent_ref);
}

void ReferenceWalk::check_prov(const std::string& owner,
                               const ProvenanceItems& prov) {
  for (const docv1::ProvenanceItem& item : prov) {
    if (item.page_no() < 1 && !page_less(item)) {
      errors_.push_back("provenance of " + owner + " has page_no "
                        + std::to_string(item.page_no())
                        + ", which is not a 1-based page");
    }
  }
}

void ReferenceWalk::collect_graph(const std::string& owner,
                                  const docv1::GraphData& graph) {
  for (const docv1::GraphCell& cell : graph.cells()) {
    if (cell.has_prov() && cell.prov().page_no() < 1
        && !page_less(cell.prov())) {
      errors_.push_back("provenance of graph cell "
                        + std::to_string(cell.cell_id()) + " of " + owner
                        + " has page_no "
                        + std::to_string(cell.prov().page_no())
                        + ", which is not a 1-based page");
    }
    if (!cell.has_item_ref()) continue;
    graph_item_refs_.emplace_back(owner, cell.item_ref().ref());
  }
}

void ReferenceWalk::walk_roots(const docv1::Document& document) {
  for (const docv1::RefItem& child : document.body().children()) {
    children_["#/body"].insert(child.ref());
  }
  for (const docv1::RefItem& child : document.furniture().children()) {
    children_["#/furniture"].insert(child.ref());
  }
  for (const docv1::GroupItem& group : document.groups()) {
    collect(group.self_ref(), group.children(), group.has_parent(),
            group.parent().ref());
  }
}

void ReferenceWalk::walk_texts(const docv1::Document& document) {
  for (const docv1::BaseTextItem& item : document.texts()) {
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      // CodeItem carries the reference fields inline instead of in a nested
      // base, so it has no TextItemBase to read; it is still a linked arena
      // item and its references are checked like every other one.
      const docv1::CodeItem& code = item.code();
      collect(code.self_ref(), code.children(), code.has_parent(),
              code.parent().ref());
      check_prov(code.self_ref(), code.prov());
      continue;
    }
    const docv1::TextItemBase* base = text_base(item);
    if (base == nullptr) {
      errors_.push_back("text item with unset variant");
      continue;
    }
    collect(base->self_ref(), base->children(), base->has_parent(),
            base->parent().ref());
    check_prov(base->self_ref(), base->prov());
  }
}

void ReferenceWalk::walk_form_arenas(const docv1::Document& document) {
  for (const docv1::KeyValueItem& item : document.key_value_items()) {
    collect(item.self_ref(), item.children(), item.has_parent(),
            item.parent().ref());
    check_prov(item.self_ref(), item.prov());
    collect_graph(item.self_ref(), item.graph());
  }
  for (const docv1::FormItem& item : document.form_items()) {
    collect(item.self_ref(), item.children(), item.has_parent(),
            item.parent().ref());
    check_prov(item.self_ref(), item.prov());
    collect_graph(item.self_ref(), item.graph());
  }
  for (const docv1::FieldRegionItem& item : document.field_regions()) {
    collect(item.self_ref(), item.children(), item.has_parent(),
            item.parent().ref());
    check_prov(item.self_ref(), item.prov());
  }
  for (const docv1::FieldItem& item : document.field_items()) {
    collect(item.self_ref(), item.children(), item.has_parent(),
            item.parent().ref());
    check_prov(item.self_ref(), item.prov());
  }
}

ReferenceWalk::ReferenceWalk(const docv1::Document& document)
    : refs_{"#/body", "#/furniture"} {
  // A page is a destination too: an outline row or a link span may point at
  // "#/pages/N", which resolves whenever the document has that page.
  for (const auto& [number, page] : document.pages()) {
    refs_.insert("#/pages/" + std::to_string(number));
  }
  walk_roots(document);
  walk_texts(document);
  for (const docv1::PictureItem& picture : document.pictures()) {
    collect(picture.self_ref(), picture.children(), picture.has_parent(),
            picture.parent().ref());
    check_prov(picture.self_ref(), picture.prov());
  }
  for (const docv1::TableItem& table : document.tables()) {
    collect(table.self_ref(), table.children(), table.has_parent(),
            table.parent().ref());
    check_prov(table.self_ref(), table.prov());
  }
  walk_form_arenas(document);
}

void ReferenceWalk::check_links() {
  for (const auto& [owner, child_refs] : children_) {
    for (const std::string& child : child_refs) {
      if (refs_.find(child) == refs_.end()) {
        errors_.push_back("child " + child + " of " + owner
                          + " does not resolve");
      }
    }
  }
  for (const auto& [child_ref, parent_ref] : parents_) {
    if (refs_.find(parent_ref) == refs_.end()) {
      errors_.push_back("parent " + parent_ref + " of " + child_ref
                        + " does not resolve");
      parent_problems_.push_back({child_ref, parent_ref, true});
      continue;
    }
    if (auto listed = children_.find(parent_ref);
        listed == children_.end()
        || listed->second.find(child_ref) == listed->second.end()) {
      errors_.push_back("parent " + parent_ref + " does not list "
                        + child_ref + " as a child");
      parent_problems_.push_back({child_ref, parent_ref, false});
    }
  }
}

void ReferenceWalk::check_resolves(const std::string& ref,
                                   const std::string& what) {
  if (!refs_.contains(ref)) errors_.push_back(what + " does not resolve");
}

void ReferenceWalk::check_anchored(const docv1::Document& document) {
  for (const docv1::TableItem& table : document.tables()) {
    for (const docv1::FineRef& comment : table.comments()) {
      check_resolves(comment.ref(),
                     "comment ref " + comment.ref() + " of "
                         + table.self_ref());
    }
  }
  for (const auto& [owner, item_ref] : graph_item_refs_) {
    check_resolves(item_ref,
                   "graph cell item_ref " + item_ref + " of " + owner);
  }
  for (const docv1::BaseTextItem& item : document.texts()) {
    const docv1::TextItemBase* base = text_base(item);
    if (base == nullptr) continue;
    for (const docv1::FineRef& comment : base->comments()) {
      check_resolves(comment.ref(),
                     "comment ref " + comment.ref() + " of "
                         + base->self_ref());
    }
    for (const docv1::InlineSpan& span : base->spans()) {
      if (!span.has_target()) continue;
      check_resolves(span.target().ref(),
                     "span target " + span.target().ref() + " of "
                         + base->self_ref());
    }
  }
  for (const docv1::ChangeRecord& change : document.changes()) {
    if (!change.has_target()) continue;
    check_resolves(change.target().ref(),
                   "change target " + change.target().ref() + " of "
                       + change.id());
  }
  for (const docv1::NamedAnchor& anchor : document.anchors()) {
    if (!anchor.has_target()) continue;
    check_resolves(anchor.target().ref(),
                   "anchor target " + anchor.target().ref() + " of "
                       + anchor.name());
  }
}

}  // namespace

std::vector<std::string> docling_integrity_errors(
    const docv1::Document& document) {
  ReferenceWalk walk(document);
  walk.check_links();
  walk.check_anchored(document);
  return walk.take_errors();
}

namespace {

// What the structural rules need to know about one node, read once.
struct NodeView {
  std::string ref;
  // docling-core's class names, for the messages.
  std::string_view kind;
  bool group = false;
  bool list_group = false;
  bool list_item = false;
  bool has_parent = false;
  std::string parent;
  std::vector<std::string> children;
};

bool is_list_group_label(docv1::GroupLabel label) {
  // ORDERED_LIST is docling-core's legacy label for the same ListGroup
  // (its deserializer builds a ListGroup for both), so both count.
  return label == docv1::GROUP_LABEL_LIST || label == docv1::GROUP_LABEL_ORDERED_LIST;
}

template <typename Item>
NodeView view_of(const Item& item, std::string_view kind) {
  NodeView view;
  view.ref = item.self_ref();
  view.kind = kind;
  view.has_parent = item.has_parent();
  view.parent = item.parent().ref();
  for (const docv1::RefItem& child : item.children()) view.children.push_back(child.ref());
  return view;
}

// Every node in arena order: the two roots, then each arena. Items with an
// empty self_ref are the integrity check's finding and are skipped here.
std::vector<NodeView> node_views(const docv1::Document& document) {
  std::vector<NodeView> views;
  NodeView body = view_of(document.body(), "GroupItem");
  body.ref = "#/body";
  body.group = true;
  views.push_back(std::move(body));
  NodeView furniture = view_of(document.furniture(), "GroupItem");
  furniture.ref = "#/furniture";
  furniture.group = true;
  views.push_back(std::move(furniture));
  for (const docv1::GroupItem& group : document.groups()) {
    NodeView view = view_of(group, is_list_group_label(group.label()) ? "ListGroup" : "GroupItem");
    view.group = true;
    view.list_group = is_list_group_label(group.label());
    views.push_back(std::move(view));
  }
  for (const docv1::BaseTextItem& item : document.texts()) {
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      views.push_back(view_of(item.code(), "CodeItem"));
      continue;
    }
    const docv1::TextItemBase* base = text_base(item);
    if (base == nullptr) continue;
    const bool list_item = item.item_case() == docv1::BaseTextItem::kListItem
        || base->label() == docv1::DOC_ITEM_LABEL_LIST_ITEM;
    NodeView view = view_of(*base, list_item ? "ListItem" : "TextItem");
    view.list_item = list_item;
    views.push_back(std::move(view));
  }
  for (const auto& item : document.pictures()) views.push_back(view_of(item, "PictureItem"));
  for (const auto& item : document.tables()) views.push_back(view_of(item, "TableItem"));
  for (const auto& item : document.key_value_items()) {
    views.push_back(view_of(item, "KeyValueItem"));
  }
  for (const auto& item : document.form_items()) views.push_back(view_of(item, "FormItem"));
  for (const auto& item : document.field_regions()) {
    views.push_back(view_of(item, "FieldRegionItem"));
  }
  for (const auto& item : document.field_items()) views.push_back(view_of(item, "FieldItem"));
  std::erase_if(views, [](const NodeView& view) { return view.ref.empty(); });
  return views;
}

}  // namespace

std::string_view structure_rule_name(StructureRule rule) {
  switch (rule) {
    case StructureRule::kDeprecatedFurnitureTree: return "deprecated_furniture_tree";
    case StructureRule::kKeyValueOrFormItem: return "key_value_or_form_item";
    case StructureRule::kListGroupNonListItemChild: return "list_group_non_list_item_child";
    case StructureRule::kListItemOutsideListGroup: return "list_item_outside_list_group";
    case StructureRule::kEmptyGroup: return "empty_group";
    case StructureRule::kParentMissing: return "parent_missing";
    case StructureRule::kNotListedByParent: return "not_listed_by_parent";
  }
  return "unknown";
}

std::vector<StructureFinding> docling_structure_findings(
    const docv1::Document& document, const std::set<StructureRule>& rules) {
  const auto checked = [&rules](StructureRule rule) {
    return rules.empty() || rules.contains(rule);
  };
  std::vector<StructureFinding> findings;
  const auto add = [&findings](StructureRule rule, std::string self_ref,
                               std::string related_ref, std::string message) {
    findings.push_back(
        {rule, std::move(self_ref), std::move(related_ref), std::move(message)});
  };

  if (checked(StructureRule::kDeprecatedFurnitureTree)
      && document.furniture().children_size() > 0) {
    add(StructureRule::kDeprecatedFurnitureTree, "#/furniture", "",
        "Deprecated furniture node #/furniture has children");
  }
  if (checked(StructureRule::kKeyValueOrFormItem)) {
    for (const docv1::KeyValueItem& item : document.key_value_items()) {
      add(StructureRule::kKeyValueOrFormItem, item.self_ref(), "",
          "Key-value item " + item.self_ref() + " is to be migrated to a field region");
    }
    for (const docv1::FormItem& item : document.form_items()) {
      add(StructureRule::kKeyValueOrFormItem, item.self_ref(), "",
          "Form item " + item.self_ref() + " is to be migrated to a field region");
    }
  }

  const std::vector<NodeView> views = node_views(document);
  std::map<std::string, const NodeView*> by_ref;
  for (const NodeView& view : views) by_ref.emplace(view.ref, &view);

  // The orphan rules are the integrity walk's parent-link findings, typed.
  ReferenceWalk walk(document);
  walk.check_links();
  std::map<std::string, std::vector<const ReferenceWalk::ParentProblem*>> problems;
  for (const auto& problem : walk.parent_problems()) {
    problems[problem.child].push_back(&problem);
  }

  for (const NodeView& view : views) {
    if (view.list_group && checked(StructureRule::kListGroupNonListItemChild)) {
      for (const std::string& child_ref : view.children) {
        const auto child = by_ref.find(child_ref);
        // A child that resolves to nothing is the integrity check's finding.
        if (child == by_ref.end() || child->second->list_item) continue;
        add(StructureRule::kListGroupNonListItemChild, view.ref, child_ref,
            "ListGroup " + view.ref + " contains non-ListItem " + child_ref + " ("
                + std::string(child->second->kind) + ")");
      }
    }
    if (view.list_item && checked(StructureRule::kListItemOutsideListGroup)) {
      if (!view.has_parent) {
        add(StructureRule::kListItemOutsideListGroup, view.ref, "",
            "ListItem " + view.ref + " has no parent");
      } else if (const auto parent = by_ref.find(view.parent);
                 parent != by_ref.end() && !parent->second->list_group) {
        add(StructureRule::kListItemOutsideListGroup, view.ref, view.parent,
            "ListItem " + view.ref + " has non-ListGroup parent: " + view.parent);
      }
    }
    if (const auto found = problems.find(view.ref); found != problems.end()) {
      for (const auto* problem : found->second) {
        const StructureRule rule = problem->missing ? StructureRule::kParentMissing
                                                    : StructureRule::kNotListedByParent;
        if (!checked(rule)) continue;
        add(rule, view.ref, problem->parent,
            problem->missing
                ? view.ref + " has non-existent parent " + problem->parent
                : view.ref + " is not a child of its parent " + problem->parent);
      }
    }
    // The roots have no parent and may be empty (an empty body is a valid
    // empty document); every other group must hold something.
    if (view.group && view.has_parent && view.children.empty()
        && checked(StructureRule::kEmptyGroup)) {
      add(StructureRule::kEmptyGroup, view.ref, "", "Group " + view.ref + " has no children");
    }
  }
  return findings;
}

}  // namespace grparse
