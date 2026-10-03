// The structural repairs docling-core #810 adds as opt-in methods
// (_migrate_furniture_to_body, _repair_referenced_orphans,
// _migrate_non_list_item_list_children, _remove_empty_groups), over the
// finished Document. Each is idempotent and returns what it changed; the
// switches live on RepairOptions and are off by default.
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "grparse/document_geometry.h"
#include "grparse/document_merge.h"
#include "grparse/document_repair.h"

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;

namespace {

using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
using Refs = google::protobuf::RepeatedPtrField<docv1::RefItem>;

constexpr std::string_view kBodyRef = "#/body";

// Every node message carries self_ref, parent, children and content_layer
// under the same names (GroupItem, TextItemBase, CodeItem and the item
// arenas), so the repairs reach them through reflection rather than one
// switch per arena.
const FieldDescriptor* field_of(const Message& node, const char* name) {
  return node.GetDescriptor()->FindFieldByName(name);
}

Refs* children_of(Message* node) {
  return node->GetReflection()->MutableRepeatedPtrField<docv1::RefItem>(
      node, field_of(*node, "children"));
}

const Refs& children_of(const Message& node) {
  return node.GetReflection()->GetRepeatedPtrField<docv1::RefItem>(
      node, field_of(node, "children"));
}

std::optional<std::string> parent_of(const Message& node) {
  const auto* field = field_of(node, "parent");
  if (!node.GetReflection()->HasField(node, field)) return std::nullopt;
  return static_cast<const docv1::RefItem&>(node.GetReflection()->GetMessage(node, field)).ref();
}

void set_parent(Message* node, std::string_view ref) {
  auto* parent = static_cast<docv1::RefItem*>(
      node->GetReflection()->MutableMessage(node, field_of(*node, "parent")));
  parent->set_ref(std::string(ref));
}

docv1::ContentLayer layer_of(const Message& node) {
  return static_cast<docv1::ContentLayer>(
      node.GetReflection()->GetEnumValue(node, field_of(node, "content_layer")));
}

void set_layer(Message* node, docv1::ContentLayer layer) {
  node->GetReflection()->SetEnumValue(node, field_of(*node, "content_layer"), layer);
}

bool lists(const Refs& children, std::string_view ref) {
  return std::ranges::any_of(children,
                             [ref](const docv1::RefItem& child) { return child.ref() == ref; });
}

struct Node {
  std::string ref;
  Message* message = nullptr;
};

template <typename Item>
void add_arena(google::protobuf::RepeatedPtrField<Item>* arena, std::string_view prefix,
               std::vector<Node>* nodes) {
  for (int index = 0; index < arena->size(); ++index) {
    Item* item = arena->Mutable(index);
    std::string ref = item->self_ref();
    if (ref.empty()) ref = std::string(prefix) + std::to_string(index);
    nodes->push_back({std::move(ref), item});
  }
}

// Every node in arena order, the way docling-core's _iterate_all_nodes
// yields them: the roots, then each arena regardless of reachability.
std::vector<Node> all_nodes(docv1::Document* document) {
  std::vector<Node> nodes;
  nodes.push_back({"#/body", document->mutable_body()});
  nodes.push_back({"#/furniture", document->mutable_furniture()});
  add_arena(document->mutable_groups(), "#/groups/", &nodes);
  for (int index = 0; index < document->texts_size(); ++index) {
    docv1::BaseTextItem* item = document->mutable_texts(index);
    Message* message = nullptr;
    std::string ref;
    if (item->item_case() == docv1::BaseTextItem::kCode) {
      message = item->mutable_code();
      ref = item->code().self_ref();
    } else if (docv1::TextItemBase* base = mutable_text_base_of(item); base != nullptr) {
      message = base;
      ref = base->self_ref();
    }
    if (message == nullptr) continue;
    if (ref.empty()) ref = "#/texts/" + std::to_string(index);
    nodes.push_back({std::move(ref), message});
  }
  add_arena(document->mutable_pictures(), "#/pictures/", &nodes);
  add_arena(document->mutable_tables(), "#/tables/", &nodes);
  add_arena(document->mutable_key_value_items(), "#/key_value_items/", &nodes);
  add_arena(document->mutable_form_items(), "#/form_items/", &nodes);
  add_arena(document->mutable_field_regions(), "#/field_regions/", &nodes);
  add_arena(document->mutable_field_items(), "#/field_items/", &nodes);
  return nodes;
}

std::map<std::string, Message*> index_nodes(const std::vector<Node>& nodes) {
  std::map<std::string, Message*> index;
  for (const Node& node : nodes) index.emplace(node.ref, node.message);
  return index;
}

// --- furniture tree migration ------------------------------------------------

// Being in the furniture tree is what marked an item as furniture, so the
// body layer becomes the furniture layer on the way out, descendants
// included; another layer (notes, invisible) is kept.
void mark_furniture(Message* node, const std::map<std::string, Message*>& index,
                    std::set<const Message*>* seen) {
  if (!seen->insert(node).second) return;
  if (layer_of(*node) == docv1::CONTENT_LAYER_BODY) {
    set_layer(node, docv1::CONTENT_LAYER_FURNITURE);
  }
  for (const docv1::RefItem& child : children_of(*node)) {
    if (const auto found = index.find(child.ref()); found != index.end()) {
      mark_furniture(found->second, index, seen);
    }
  }
}

struct Moved {
  docv1::RefItem ref;
  bool footer = false;
  // Zero when the item names no usable page.
  int page = 0;
  // docling-core's stable visual order inside a page: the 12-point line
  // band, then the left edge, then the source order.
  std::tuple<long, double, int> key{0, 0.0, 0};
};

bool moved_before(const Moved& a, const Moved& b) {
  return std::tie(a.page, a.footer, a.key) < std::tie(b.page, b.footer, b.key);
}

}  // namespace

int migrate_furniture_tree(docv1::Document* document) {
  if (document->furniture().children_size() == 0) return 0;
  const std::vector<Node> nodes = all_nodes(document);
  const std::map<std::string, Message*> index = index_nodes(nodes);
  const std::map<int, double> heights = document_page_heights(*document);

  std::vector<Moved> leading;   // unlocated headers, source order
  std::vector<Moved> located;   // headers and footers with a page
  std::vector<Moved> trailing;  // unlocated footers, source order
  std::set<const Message*> seen;
  const auto& furniture = document->furniture().children();
  for (int position = 0; position < furniture.size(); ++position) {
    Moved moved;
    moved.ref = furniture[position];
    const std::optional<ItemPlacement> placement =
        item_placement(*document, moved.ref.ref(), heights);
    // The label says header or footer outright where the producer named
    // one; docling-core only has geometry, which decides the rest here
    // too: a box centred in the lower half of its page is a footer.
    const docv1::DocItemLabel label = item_label(*document, moved.ref.ref());
    if (label == docv1::DOC_ITEM_LABEL_PAGE_FOOTER) {
      moved.footer = true;
    } else if (label != docv1::DOC_ITEM_LABEL_PAGE_HEADER && placement.has_value()) {
      const double height = heights.at(placement->page);
      moved.footer = (placement->box.top + placement->box.bottom) / 2.0 > height / 2.0;
    }
    if (placement.has_value()) {
      moved.page = placement->page;
      moved.key = {std::lround(placement->box.top / 12.0), placement->box.left, position};
      located.push_back(std::move(moved));
    } else {
      moved.key = {0, 0.0, position};
      (moved.footer ? trailing : leading).push_back(std::move(moved));
    }
    if (const auto found = index.find(furniture[position].ref()); found != index.end()) {
      set_parent(found->second, kBodyRef);
      mark_furniture(found->second, index, &seen);
    }
  }
  std::ranges::stable_sort(located, moved_before);

  // docling-core puts every header before the whole body and every footer
  // after it, so a multi-page document reads all its running heads first.
  // Here a located header goes before the first body item of its page and
  // a footer after the last, so the body stays in page order; only when no
  // body item names a page does the docling-core placement apply as is.
  const Refs& body = document->body().children();
  std::vector<std::optional<int>> body_pages;
  bool any_page = false;
  for (const docv1::RefItem& child : body) {
    const auto placement = item_placement(*document, child.ref(), heights);
    body_pages.push_back(placement ? std::optional<int>(placement->page) : std::nullopt);
    any_page = any_page || placement.has_value();
  }
  Refs merged;
  for (const Moved& moved : leading) *merged.Add() = moved.ref;
  if (!any_page) {
    for (const Moved& moved : located) {
      if (!moved.footer) *merged.Add() = moved.ref;
    }
    for (const docv1::RefItem& child : body) *merged.Add() = child;
    for (const Moved& moved : located) {
      if (moved.footer) *merged.Add() = moved.ref;
    }
  } else {
    size_t next = 0;
    for (int position = 0; position < body.size(); ++position) {
      if (body_pages[position].has_value()) {
        const int page = *body_pages[position];
        while (next < located.size() &&
               (located[next].page < page ||
                (located[next].page == page && !located[next].footer))) {
          *merged.Add() = located[next++].ref;
        }
      }
      *merged.Add() = body[position];
    }
    for (; next < located.size(); ++next) *merged.Add() = located[next].ref;
  }
  for (const Moved& moved : trailing) *merged.Add() = moved.ref;

  const int migrated = document->furniture().children_size();
  document->mutable_body()->mutable_children()->Swap(&merged);
  document->mutable_furniture()->clear_children();
  return migrated;
}

int repair_referenced_orphans(docv1::Document* document) {
  const std::vector<Node> nodes = all_nodes(document);
  const std::map<std::string, Message*> index = index_nodes(nodes);
  int repaired = 0;
  for (const Node& node : nodes) {
    // The floating items: the ones that carry captions, footnotes and
    // references (pictures, tables, code, key-value and form items).
    const auto* captions = field_of(*node.message, "captions");
    if (captions == nullptr) continue;
    std::vector<std::string> named;
    for (const char* name : {"captions", "footnotes", "references"}) {
      for (const docv1::RefItem& ref :
           node.message->GetReflection()->GetRepeatedPtrField<docv1::RefItem>(
               *node.message, field_of(*node.message, name))) {
        named.push_back(ref.ref());
      }
    }
    for (const std::string& ref : named) {
      const auto item = index.find(ref);
      if (item == index.end()) continue;
      if (parent_of(*item->second) != node.ref) continue;
      Refs* children = children_of(node.message);
      if (lists(*children, ref)) continue;
      children->Add()->set_ref(ref);
      ++repaired;
    }
  }
  return repaired;
}

namespace {

// The list items of the texts arena by reference, each with its enumerated
// flag when it is the ListItem variant (a plain text labelled LIST_ITEM has
// none).
std::map<std::string, std::optional<bool>> list_items_of(const docv1::Document& document) {
  std::map<std::string, std::optional<bool>> items;
  for (int index = 0; index < document.texts_size(); ++index) {
    const docv1::BaseTextItem& item = document.texts(index);
    const docv1::TextItemBase* base = text_base_of(item);
    if (base == nullptr) continue;
    const std::string ref = base->self_ref().empty() ? "#/texts/" + std::to_string(index)
                                                     : base->self_ref();
    if (item.item_case() == docv1::BaseTextItem::kListItem) {
      items.emplace(ref, item.list_item().enumerated());
    } else if (base->label() == docv1::DOC_ITEM_LABEL_LIST_ITEM) {
      items.emplace(ref, std::nullopt);
    }
  }
  return items;
}

bool is_list_group(const docv1::GroupItem& group) {
  return group.label() == docv1::GROUP_LABEL_LIST ||
         group.label() == docv1::GROUP_LABEL_ORDERED_LIST;
}

std::string group_ref(const docv1::GroupItem& group, int index) {
  return group.self_ref().empty() ? "#/groups/" + std::to_string(index) : group.self_ref();
}

}  // namespace

int wrap_list_children(docv1::Document* document) {
  struct Wrap {
    int group = 0;
    std::string child;
  };
  std::vector<Wrap> wraps;
  {
    const std::vector<Node> nodes = all_nodes(document);
    const std::map<std::string, Message*> index = index_nodes(nodes);
    const auto list_items = list_items_of(*document);
    for (int group = 0; group < document->groups_size(); ++group) {
      const docv1::GroupItem& list = document->groups(group);
      if (!is_list_group(list)) continue;
      const std::string list_ref = group_ref(list, group);
      for (const docv1::RefItem& child : list.children()) {
        const auto found = index.find(child.ref());
        if (found == index.end() || list_items.contains(child.ref())) continue;
        // A child that names another parent is an orphan problem, not this
        // repair's; docling-core skips it the same way.
        if (parent_of(*found->second) != list_ref) continue;
        wraps.push_back({group, child.ref()});
      }
    }
  }
  for (const Wrap& wrap : wraps) {
    // Adding a text item never moves the others, but the index is rebuilt
    // per wrap so no handle outlives an arena append.
    const std::vector<Node> nodes = all_nodes(document);
    Message* child = index_nodes(nodes).at(wrap.child);
    const auto list_items = list_items_of(*document);
    docv1::GroupItem* list = document->mutable_groups(wrap.group);
    const std::string list_ref = group_ref(*list, wrap.group);
    // docling-core leaves the new item unenumerated; here it follows its
    // siblings, or the group's ordered label when it has none, so a
    // renderer numbers the wrapped child with the rest of its list.
    std::optional<bool> enumerated;
    for (const docv1::RefItem& sibling : list->children()) {
      const auto found = list_items.find(sibling.ref());
      if (found != list_items.end() && found->second.has_value()) {
        enumerated = found->second;
        break;
      }
    }
    const docv1::ContentLayer layer = layer_of(*child);
    const std::string item_ref = "#/texts/" + std::to_string(document->texts_size());
    set_parent(child, item_ref);
    docv1::ListItem* item = document->add_texts()->mutable_list_item();
    item->set_enumerated(enumerated.value_or(list->label() == docv1::GROUP_LABEL_ORDERED_LIST));
    item->set_marker("");
    docv1::TextItemBase* base = item->mutable_base();
    base->set_self_ref(item_ref);
    base->mutable_parent()->set_ref(list_ref);
    base->set_label(docv1::DOC_ITEM_LABEL_LIST_ITEM);
    base->set_content_layer(layer);
    base->add_children()->set_ref(wrap.child);
    for (docv1::RefItem& entry : *list->mutable_children()) {
      if (entry.ref() == wrap.child) {
        entry.set_ref(item_ref);
        break;
      }
    }
  }
  return static_cast<int>(wraps.size());
}

int remove_empty_groups(docv1::Document* document) {
  int removed_total = 0;
  while (true) {
    const std::vector<Node> nodes = all_nodes(document);
    std::set<std::string> claimed;
    for (const Node& node : nodes) {
      if (const auto parent = parent_of(*node.message); parent.has_value()) {
        claimed.insert(*parent);
      }
    }
    std::map<std::string, std::string> removed;  // group ref -> its parent
    for (int index = 0; index < document->groups_size(); ++index) {
      const docv1::GroupItem& group = document->groups(index);
      const std::string ref = group_ref(group, index);
      if (group.has_parent() && group.children_size() == 0 && !claimed.contains(ref)) {
        removed.emplace(ref, group.parent().ref());
      }
    }
    if (removed.empty()) return removed_total;

    for (const Node& node : nodes) {
      Refs* children = children_of(node.message);
      children->erase(std::remove_if(children->begin(), children->end(),
                                     [&removed](const docv1::RefItem& child) {
                                       return removed.contains(child.ref());
                                     }),
                      children->end());
    }
    std::map<std::string, std::string> renumbering;
    google::protobuf::RepeatedPtrField<docv1::GroupItem> kept;
    for (int index = 0; index < document->groups_size(); ++index) {
      docv1::GroupItem* group = document->mutable_groups(index);
      const std::string old_ref = group_ref(*group, index);
      if (removed.contains(old_ref)) continue;
      const std::string new_ref = "#/groups/" + std::to_string(kept.size());
      if (old_ref != new_ref) renumbering[old_ref] = new_ref;
      *kept.Add() = std::move(*group);
    }
    document->mutable_groups()->Swap(&kept);
    // A reference into a removed group (an anchor, a span target) follows
    // it to its parent, the way retire_text_items sends one into a retired
    // item to the item that absorbed it; renumbering the survivors after
    // the removal must never land such a reference on another group.
    for (const auto& [ref, parent] : removed) {
      const auto renamed = renumbering.find(parent);
      renumbering[ref] = renamed == renumbering.end() ? parent : renamed->second;
    }
    rewrite_references(renumbering, document);
    removed_total += static_cast<int>(removed.size());
  }
}

}  // namespace grparse
