#include "grparse/document_geometry.h"

#include <algorithm>
#include <charconv>
#include <set>
#include <string>

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;

namespace {

// The arena and index a JSON Pointer reference names ("#/texts/12").
struct ArenaSlot {
  std::string_view arena;
  int index = -1;
};

std::optional<ArenaSlot> arena_slot(std::string_view ref) {
  if (!ref.starts_with("#/")) return std::nullopt;
  const std::string_view rest = ref.substr(2);
  const size_t slash = rest.find('/');
  if (slash == std::string_view::npos) return std::nullopt;
  const std::string_view digits = rest.substr(slash + 1);
  int index = -1;
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
  if (error != std::errc() || end != digits.data() + digits.size() || index < 0) return std::nullopt;
  return ArenaSlot{rest.substr(0, slash), index};
}

void grow(TopDownBox* box, const TopDownBox& other, bool* any) {
  if (!*any) {
    *box = other;
    *any = true;
    return;
  }
  box->left = std::min(box->left, other.left);
  box->top = std::min(box->top, other.top);
  box->right = std::max(box->right, other.right);
  box->bottom = std::max(box->bottom, other.bottom);
}

// The union of an item's boxes on `page`, when it has any with area.
std::optional<TopDownBox> union_on_page(
    const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& prov, int page,
    double page_height) {
  TopDownBox box;
  bool any = false;
  for (const auto& entry : prov) {
    if (entry.page_no() != page || !entry.has_bbox()) continue;
    const TopDownBox candidate = top_down_box(entry.bbox(), page_height);
    if (candidate.width() <= 0 || candidate.height() <= 0) continue;
    grow(&box, candidate, &any);
  }
  if (!any) return std::nullopt;
  return box;
}

std::optional<ItemPlacement> placement_of(
    const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& prov,
    const std::map<int, double>& page_heights) {
  const int page = first_page_of(prov);
  const auto height = page_heights.find(page);
  if (page <= 0 || height == page_heights.end()) return std::nullopt;
  const auto box = union_on_page(prov, page, height->second);
  if (!box.has_value()) return std::nullopt;
  return ItemPlacement{page, *box};
}

std::optional<ItemPlacement> group_placement(const docv1::Document& document,
                                             const docv1::GroupItem& group,
                                             const std::map<int, double>& page_heights, int depth) {
  // Groups nest; a cycle or an absurd depth ends the walk instead of the
  // stack.
  if (depth > 16) return std::nullopt;
  std::optional<ItemPlacement> placement;
  for (const auto& child : group.children()) {
    std::optional<ItemPlacement> member;
    if (const auto slot = arena_slot(child.ref());
        slot.has_value() && slot->arena == "groups" && slot->index < document.groups_size()) {
      member = group_placement(document, document.groups(slot->index), page_heights, depth + 1);
    } else {
      member = item_placement(document, child.ref(), page_heights);
    }
    if (!member.has_value()) continue;
    if (!placement.has_value() || member->page < placement->page) {
      placement = member;
    } else if (member->page == placement->page) {
      bool any = true;
      grow(&placement->box, member->box, &any);
    }
  }
  return placement;
}

void note_extent(const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& prov,
                 std::map<int, double>* extents) {
  for (const auto& entry : prov) {
    if (entry.page_no() <= 0 || !entry.has_bbox()) continue;
    auto& extent = (*extents)[entry.page_no()];
    extent = std::max({extent, entry.bbox().t(), entry.bbox().b()});
  }
}

template <typename Item>
void note_collectors(const Item& item, std::set<std::string>* names) {
  for (const auto& source : item.source()) {
    if (source.has_collector()) names->insert(source.collector().collector());
  }
}

}  // namespace

const docv1::TextItemBase* text_base_of(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle: return &item.title().base();
    case docv1::BaseTextItem::kSectionHeader: return &item.section_header().base();
    case docv1::BaseTextItem::kListItem: return &item.list_item().base();
    case docv1::BaseTextItem::kFormula: return &item.formula().base();
    case docv1::BaseTextItem::kText: return &item.text().base();
    case docv1::BaseTextItem::kFieldHeading: return &item.field_heading().base();
    case docv1::BaseTextItem::kFieldValue: return &item.field_value().base();
    case docv1::BaseTextItem::kCode:
    case docv1::BaseTextItem::ITEM_NOT_SET: return nullptr;
  }
  return nullptr;
}

docv1::TextItemBase* mutable_text_base_of(docv1::BaseTextItem* item) {
  switch (item->item_case()) {
    case docv1::BaseTextItem::kTitle: return item->mutable_title()->mutable_base();
    case docv1::BaseTextItem::kSectionHeader: return item->mutable_section_header()->mutable_base();
    case docv1::BaseTextItem::kListItem: return item->mutable_list_item()->mutable_base();
    case docv1::BaseTextItem::kFormula: return item->mutable_formula()->mutable_base();
    case docv1::BaseTextItem::kText: return item->mutable_text()->mutable_base();
    case docv1::BaseTextItem::kFieldHeading: return item->mutable_field_heading()->mutable_base();
    case docv1::BaseTextItem::kFieldValue: return item->mutable_field_value()->mutable_base();
    case docv1::BaseTextItem::kCode:
    case docv1::BaseTextItem::ITEM_NOT_SET: return nullptr;
  }
  return nullptr;
}

int first_page_of(const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& prov) {
  int page = 0;
  for (const auto& entry : prov) {
    if (entry.page_no() > 0 && (page == 0 || entry.page_no() < page)) page = entry.page_no();
  }
  return page;
}

std::map<int, double> document_page_heights(const docv1::Document& document) {
  std::map<int, double> heights;
  for (const auto& [page_no, page] : document.pages()) {
    if (page.size().height() > 0) heights[page_no] = page.size().height();
  }
  std::map<int, double> extents;
  for (const auto& item : document.texts()) {
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      note_extent(item.code().prov(), &extents);
    } else if (const auto* base = text_base_of(item); base != nullptr) {
      note_extent(base->prov(), &extents);
    }
  }
  for (const auto& table : document.tables()) note_extent(table.prov(), &extents);
  for (const auto& picture : document.pictures()) note_extent(picture.prov(), &extents);
  for (const auto& [page_no, extent] : extents) {
    if (!heights.contains(page_no) && extent > 0) heights[page_no] = extent;
  }
  return heights;
}

TopDownBox top_down_box(const docv1::BoundingBox& box, double page_height) {
  const double left = std::min(box.l(), box.r());
  const double right = std::max(box.l(), box.r());
  if (box.coord_origin() == docv1::COORD_ORIGIN_BOTTOMLEFT) {
    return {left, page_height - std::max(box.t(), box.b()), right,
            page_height - std::min(box.t(), box.b())};
  }
  return {left, std::min(box.t(), box.b()), right, std::max(box.t(), box.b())};
}

std::optional<ItemPlacement> provenance_placement(
    const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& prov,
    const std::map<int, double>& page_heights) {
  return placement_of(prov, page_heights);
}

std::optional<ItemPlacement> item_placement(const docv1::Document& document, std::string_view ref,
                                            const std::map<int, double>& page_heights) {
  const auto slot = arena_slot(ref);
  if (!slot.has_value()) return std::nullopt;
  if (slot->arena == "texts" && slot->index < document.texts_size()) {
    const auto& item = document.texts(slot->index);
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      return placement_of(item.code().prov(), page_heights);
    }
    const auto* base = text_base_of(item);
    return base == nullptr ? std::nullopt : placement_of(base->prov(), page_heights);
  }
  if (slot->arena == "tables" && slot->index < document.tables_size()) {
    return placement_of(document.tables(slot->index).prov(), page_heights);
  }
  if (slot->arena == "pictures" && slot->index < document.pictures_size()) {
    return placement_of(document.pictures(slot->index).prov(), page_heights);
  }
  if (slot->arena == "groups" && slot->index < document.groups_size()) {
    return group_placement(document, document.groups(slot->index), page_heights, 0);
  }
  return std::nullopt;
}

docv1::DocItemLabel item_label(const docv1::Document& document, std::string_view ref) {
  const auto slot = arena_slot(ref);
  if (!slot.has_value()) return docv1::DOC_ITEM_LABEL_UNSPECIFIED;
  if (slot->arena == "texts" && slot->index < document.texts_size()) {
    const auto& item = document.texts(slot->index);
    if (item.item_case() == docv1::BaseTextItem::kCode) return item.code().label();
    const auto* base = text_base_of(item);
    return base == nullptr ? docv1::DOC_ITEM_LABEL_UNSPECIFIED : base->label();
  }
  if (slot->arena == "tables" && slot->index < document.tables_size()) {
    return docv1::DOC_ITEM_LABEL_TABLE;
  }
  if (slot->arena == "pictures" && slot->index < document.pictures_size()) {
    return docv1::DOC_ITEM_LABEL_PICTURE;
  }
  return docv1::DOC_ITEM_LABEL_UNSPECIFIED;
}

bool produced_only_by(const docv1::Document& document, const std::vector<std::string>& collectors) {
  std::set<std::string> names;
  for (const auto& item : document.texts()) {
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      note_collectors(item.code(), &names);
    } else if (const auto* base = text_base_of(item); base != nullptr) {
      note_collectors(*base, &names);
    }
  }
  for (const auto& table : document.tables()) note_collectors(table, &names);
  for (const auto& picture : document.pictures()) note_collectors(picture, &names);
  if (names.empty()) return false;
  return std::ranges::all_of(names, [&collectors](const std::string& name) {
    return std::ranges::find(collectors, name) != collectors.end();
  });
}

}  // namespace grparse
