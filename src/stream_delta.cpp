#include "grparse/stream_delta.h"

#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <google/protobuf/util/message_differencer.h>

#include "grparse/document_geometry.h"
#include "grparse/document_merge.h"

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

namespace {

constexpr std::string_view kTexts = "#/texts/";
constexpr std::string_view kTables = "#/tables/";
constexpr std::string_view kPictures = "#/pictures/";
constexpr std::string_view kGroups = "#/groups/";

// How far past an arena's end a self_ref may place an item. A page event
// names its items by document-global index, so the gap a missing page
// leaves is bounded by that page's items; a wild index appends instead of
// allocating up to it.
constexpr int kMaximumGap = 1 << 20;

std::optional<int> arena_index(std::string_view ref, std::string_view prefix) {
  if (!ref.starts_with(prefix)) return std::nullopt;
  int index = 0;
  const char* last = ref.data() + ref.size();
  const auto [end, error] = std::from_chars(ref.data() + prefix.size(), last, index);
  if (error != std::errc() || end != last || index < 0) return std::nullopt;
  return index;
}

std::string self_ref_of(const docv1::BaseTextItem& item) {
  if (item.item_case() == docv1::BaseTextItem::kCode) return item.code().self_ref();
  const auto* base = text_base_of(item);
  return base == nullptr ? std::string() : base->self_ref();
}

std::string parent_of(const docv1::BaseTextItem& item) {
  if (item.item_case() == docv1::BaseTextItem::kCode) {
    return item.code().has_parent() ? item.code().parent().ref() : std::string();
  }
  const auto* base = text_base_of(item);
  return base == nullptr || !base->has_parent() ? std::string() : base->parent().ref();
}

template <typename Item>
std::string self_ref_of(const Item& item) {
  return item.self_ref();
}

template <typename Item>
std::string parent_of(const Item& item) {
  return item.has_parent() ? item.parent().ref() : std::string();
}

template <typename Item>
std::optional<std::string> parent_in(const google::protobuf::RepeatedPtrField<Item>& arena,
                                     const std::string& ref, std::string_view prefix) {
  const auto index = arena_index(ref, prefix);
  if (!index.has_value() || *index >= arena.size() || self_ref_of(arena[*index]) != ref) {
    return std::nullopt;
  }
  return parent_of(arena[*index]);
}

// The parent ref of the arena item `ref` names, or nothing when no item in
// the document carries that ref (#/body, #/furniture, a slot not folded).
std::optional<std::string> parent_in(const docv1::Document& document, const std::string& ref) {
  if (auto parent = parent_in(document.texts(), ref, kTexts)) return parent;
  if (auto parent = parent_in(document.tables(), ref, kTables)) return parent;
  if (auto parent = parent_in(document.pictures(), ref, kPictures)) return parent;
  return parent_in(document.groups(), ref, kGroups);
}

// The ancestor of `ref` whose parent is no arena item, and that parent:
// the top-level child a tree root lists. A cycle stops at the depth limit.
std::pair<std::string, std::string> top_level(const docv1::Document& document,
                                              std::string ref) {
  for (int depth = 0; depth < 256; ++depth) {
    const auto parent = parent_in(document, ref);
    if (!parent.has_value()) return {std::move(ref), std::string()};
    if (!parent_in(document, *parent).has_value()) return {std::move(ref), *parent};
    ref = *parent;
  }
  return {std::move(ref), std::string()};
}

template <typename Item>
void place(google::protobuf::RepeatedPtrField<Item>* arena, const Item& item,
           std::string_view prefix) {
  const auto index = arena_index(self_ref_of(item), prefix);
  if (!index.has_value() || *index - arena->size() > kMaximumGap) {
    *arena->Add() = item;
    return;
  }
  while (arena->size() <= *index) arena->Add();
  *arena->Mutable(*index) = item;
}

bool same_refs(const google::protobuf::RepeatedPtrField<docv1::RefItem>& left,
               const google::protobuf::RepeatedPtrField<docv1::RefItem>& right) {
  if (left.size() != right.size()) return false;
  for (int index = 0; index < left.size(); ++index) {
    if (left[index].ref() != right[index].ref()) return false;
  }
  return true;
}

// Items of `after` that the renamed fold does not already hold verbatim
// under the same ref, and only that ref.
template <typename Item>
bool diff_arena(const google::protobuf::RepeatedPtrField<Item>& before,
                const google::protobuf::RepeatedPtrField<Item>& after,
                google::protobuf::RepeatedPtrField<Item>* changed) {
  std::unordered_map<std::string, int> slot;
  std::unordered_set<std::string> shared;
  for (int index = 0; index < before.size(); ++index) {
    const std::string ref = self_ref_of(before[index]);
    if (!slot.emplace(ref, index).second) shared.insert(ref);
  }
  bool any = false;
  for (const Item& item : after) {
    const std::string ref = self_ref_of(item);
    const auto found = slot.find(ref);
    if (found != slot.end() && !shared.contains(ref) &&
        google::protobuf::util::MessageDifferencer::Equals(before[found->second], item)) {
      continue;
    }
    *changed->Add() = item;
    any = true;
  }
  return any;
}

// Rebuilds one arena at `count`, slot N from the upsert naming N or the
// one folded item now carrying N's ref.
template <typename Item>
bool rebuild(google::protobuf::RepeatedPtrField<Item>* arena,
             const google::protobuf::RepeatedPtrField<Item>& upserts, uint32_t count,
             std::string_view prefix) {
  std::map<int, const Item*> upserted;
  for (const Item& item : upserts) {
    const auto index = arena_index(self_ref_of(item), prefix);
    if (!index.has_value() || static_cast<uint32_t>(*index) >= count) return false;
    upserted[*index] = &item;
  }
  std::unordered_map<std::string, int> slot;
  std::unordered_set<std::string> shared;
  for (int index = 0; index < arena->size(); ++index) {
    const std::string ref = self_ref_of((*arena)[index]);
    if (!slot.emplace(ref, index).second) shared.insert(ref);
  }
  google::protobuf::RepeatedPtrField<Item> next;
  next.Reserve(static_cast<int>(count));
  for (uint32_t index = 0; index < count; ++index) {
    if (const auto found = upserted.find(static_cast<int>(index)); found != upserted.end()) {
      *next.Add() = *found->second;
      continue;
    }
    const std::string ref = std::string(prefix) + std::to_string(index);
    const auto found = slot.find(ref);
    if (found == slot.end() || shared.contains(ref)) return false;
    *next.Add() = std::move(*arena->Mutable(found->second));
  }
  arena->Swap(&next);
  return true;
}

}  // namespace

void PageFold::fold(const parsev1::PageData& page) {
  ++pages_;
  for (const auto& group : page.groups()) place(document_.mutable_groups(), group, kGroups);
  for (const auto& text : page.texts()) place(document_.mutable_texts(), text, kTexts);
  for (const auto& table : page.tables()) place(document_.mutable_tables(), table, kTables);
  for (const auto& picture : page.pictures()) {
    place(document_.mutable_pictures(), picture, kPictures);
  }
  (*document_.mutable_pages())[page.page_number()] = page.page_meta();
  std::unordered_set<std::string> in_body;
  for (const auto& child : page.body_order()) {
    in_body.insert(child.ref());
    auto [top, root] = top_level(document_, child.ref());
    if (root == "#/furniture") continue;
    if (body_.insert(top).second) document_.mutable_body()->add_children()->set_ref(top);
  }
  const auto furnish = [this, &in_body](const std::string& ref) {
    if (in_body.contains(ref)) return;
    auto [top, root] = top_level(document_, ref);
    if (root != "#/furniture") return;
    if (furniture_.insert(top).second) {
      document_.mutable_furniture()->add_children()->set_ref(top);
    }
  };
  for (const auto& group : page.groups()) furnish(group.self_ref());
  for (const auto& text : page.texts()) furnish(self_ref_of(text));
  for (const auto& table : page.tables()) furnish(table.self_ref());
  for (const auto& picture : page.pictures()) furnish(picture.self_ref());
}

void apply_section_header_levels(const std::map<std::string, int32_t>& levels,
                                 docv1::Document* document) {
  if (levels.empty()) return;
  for (auto& text : *document->mutable_texts()) {
    if (text.item_case() != docv1::BaseTextItem::kSectionHeader) continue;
    auto* header = text.mutable_section_header();
    if (const auto level = levels.find(header->base().self_ref()); level != levels.end()) {
      header->set_level(level->second);
    }
  }
}

std::optional<parsev1::DocumentRepairDelta> repair_delta(
    const docv1::Document& before, const docv1::Document& after,
    const std::map<std::string, std::string>& renamed, parsev1::Collector collector) {
  docv1::Document fold = before;
  if (!renamed.empty()) rewrite_references(renamed, &fold);
  parsev1::DocumentRepairDelta delta;
  delta.set_collector(collector);
  bool changed = !renamed.empty();
  delta.mutable_renamed_refs()->insert(renamed.begin(), renamed.end());
  changed |= diff_arena(fold.texts(), after.texts(), delta.mutable_texts());
  changed |= diff_arena(fold.tables(), after.tables(), delta.mutable_tables());
  changed |= diff_arena(fold.pictures(), after.pictures(), delta.mutable_pictures());
  changed |= diff_arena(fold.groups(), after.groups(), delta.mutable_groups());
  delta.set_text_count(static_cast<uint32_t>(after.texts_size()));
  delta.set_table_count(static_cast<uint32_t>(after.tables_size()));
  delta.set_picture_count(static_cast<uint32_t>(after.pictures_size()));
  delta.set_group_count(static_cast<uint32_t>(after.groups_size()));
  changed |= fold.texts_size() != after.texts_size() ||
             fold.tables_size() != after.tables_size() ||
             fold.pictures_size() != after.pictures_size() ||
             fold.groups_size() != after.groups_size();
  if (!same_refs(fold.body().children(), after.body().children())) {
    *delta.mutable_body_children()->mutable_refs() = after.body().children();
    changed = true;
  }
  if (!same_refs(fold.furniture().children(), after.furniture().children())) {
    *delta.mutable_furniture_children()->mutable_refs() = after.furniture().children();
    changed = true;
  }
  for (const auto& [page_no, page] : after.pages()) {
    const auto folded = fold.pages().find(page_no);
    if (folded == fold.pages().end() ||
        !google::protobuf::util::MessageDifferencer::Equals(folded->second, page)) {
      (*delta.mutable_pages())[page_no] = page;
      changed = true;
    }
  }
  for (const auto& [page_no, _] : fold.pages()) {
    if (!after.pages().contains(page_no)) {
      delta.add_dropped_pages(page_no);
      changed = true;
    }
  }
  if (!changed) return std::nullopt;
  return delta;
}

bool apply_repair_delta(const parsev1::DocumentRepairDelta& delta, docv1::Document* document) {
  if (!delta.renamed_refs().empty()) {
    const std::map<std::string, std::string> renamed(delta.renamed_refs().begin(),
                                                     delta.renamed_refs().end());
    rewrite_references(renamed, document);
  }
  if (!rebuild(document->mutable_texts(), delta.texts(), delta.text_count(), kTexts) ||
      !rebuild(document->mutable_tables(), delta.tables(), delta.table_count(), kTables) ||
      !rebuild(document->mutable_pictures(), delta.pictures(), delta.picture_count(),
               kPictures) ||
      !rebuild(document->mutable_groups(), delta.groups(), delta.group_count(), kGroups)) {
    return false;
  }
  if (delta.has_body_children()) {
    *document->mutable_body()->mutable_children() = delta.body_children().refs();
  }
  if (delta.has_furniture_children()) {
    *document->mutable_furniture()->mutable_children() = delta.furniture_children().refs();
  }
  for (const auto& [page_no, page] : delta.pages()) (*document->mutable_pages())[page_no] = page;
  for (const int32_t page_no : delta.dropped_pages()) document->mutable_pages()->erase(page_no);
  return true;
}

}  // namespace grparse
