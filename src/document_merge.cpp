#include "grparse/document_merge.h"

#include <map>
#include <string>
#include <utility>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

using RefMap = std::map<std::string, std::string>;

// Rewrites every reference the moved items carry. References live in two
// string shapes: RefItem.ref / FineRef.ref pointers to other items, and an
// item's own self_ref. Walking by reflection keeps the merge correct when
// the schema grows new item kinds or new reference fields, instead of
// silently missing them in a hand-maintained field list. Only values that
// name a moved item are touched, so "#/body", "#/furniture", and
// already-merged references pass through unchanged.
void rewrite_refs(const RefMap& mapping, google::protobuf::Message* message) {
  const auto* descriptor = message->GetDescriptor();
  const auto* reflection = message->GetReflection();
  for (int index = 0; index < descriptor->field_count(); ++index) {
    const auto* field = descriptor->field(index);
    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING &&
        !field->is_repeated() &&
        (field->name() == "ref" || field->name() == "self_ref")) {
      if (field->has_presence() && !reflection->HasField(*message, field)) continue;
      const auto mapped = mapping.find(reflection->GetString(*message, field));
      if (mapped != mapping.end()) {
        reflection->SetString(message, field, mapped->second);
      }
      continue;
    }
    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }
    if (field->is_repeated()) {
      // Repeated covers maps too: a map field walks as its entry messages,
      // whose values recurse here when they are messages themselves.
      const int size = reflection->FieldSize(*message, field);
      for (int entry = 0; entry < size; ++entry) {
        rewrite_refs(mapping, reflection->MutableRepeatedMessage(message, field, entry));
      }
      continue;
    }
    if (reflection->HasField(*message, field)) {
      rewrite_refs(mapping, reflection->MutableMessage(message, field));
    }
  }
}

// The self_ref an arena item currently carries; text items keep it inside
// their variant's base.
std::string item_self_ref(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle: return item.title().base().self_ref();
    case docv1::BaseTextItem::kSectionHeader:
      return item.section_header().base().self_ref();
    case docv1::BaseTextItem::kListItem: return item.list_item().base().self_ref();
    case docv1::BaseTextItem::kFormula: return item.formula().base().self_ref();
    case docv1::BaseTextItem::kText: return item.text().base().self_ref();
    // CodeItem carries its fields inline instead of a nested base.
    case docv1::BaseTextItem::kCode: return item.code().self_ref();
    case docv1::BaseTextItem::kFieldHeading:
      return item.field_heading().base().self_ref();
    case docv1::BaseTextItem::kFieldValue:
      return item.field_value().base().self_ref();
    case docv1::BaseTextItem::ITEM_NOT_SET: return std::string();
  }
  return std::string();
}

// Registers the renumbering for one arena: source item i becomes target
// item (existing + i) under the arena's JSON Pointer prefix.
template <typename Arena, typename SelfRefOf>
void map_arena(const Arena& source_items, int existing, const std::string& prefix,
               SelfRefOf self_ref_of, RefMap* mapping) {
  for (int index = 0; index < source_items.size(); ++index) {
    std::string old_ref = self_ref_of(source_items.Get(index));
    if (old_ref.empty()) old_ref = prefix + std::to_string(index);
    (*mapping)[old_ref] = prefix + std::to_string(existing + index);
  }
}

// Appends the source root group's children and metadata to the target's.
// Children arrive already rewritten.
void merge_root_group(docv1::GroupItem&& source, docv1::GroupItem* target) {
  for (auto& child : *source.mutable_children()) {
    *target->add_children() = std::move(child);
  }
  if (source.has_meta()) {
    auto& fields = *target->mutable_meta()->mutable_custom_fields();
    for (auto& entry : *source.mutable_meta()->mutable_custom_fields()) {
      // Additive: an existing key wins, a new key lands.
      fields.emplace(entry.first, std::move(entry.second));
    }
  }
}

const auto self_ref_field = [](const auto& item) { return item.self_ref(); };

}  // namespace

void merge_documents(docv1::Document&& source, docv1::Document* target) {
  RefMap mapping;
  map_arena(source.groups(), target->groups_size(), "#/groups/", self_ref_field,
            &mapping);
  map_arena(source.texts(), target->texts_size(), "#/texts/", item_self_ref,
            &mapping);
  map_arena(source.pictures(), target->pictures_size(), "#/pictures/",
            self_ref_field, &mapping);
  map_arena(source.tables(), target->tables_size(), "#/tables/", self_ref_field,
            &mapping);
  map_arena(source.key_value_items(), target->key_value_items_size(),
            "#/key_value_items/", self_ref_field, &mapping);
  map_arena(source.form_items(), target->form_items_size(), "#/form_items/",
            self_ref_field, &mapping);

  rewrite_refs(mapping, &source);

  for (auto& item : *source.mutable_groups()) *target->add_groups() = std::move(item);
  for (auto& item : *source.mutable_texts()) *target->add_texts() = std::move(item);
  for (auto& item : *source.mutable_pictures()) *target->add_pictures() = std::move(item);
  for (auto& item : *source.mutable_tables()) *target->add_tables() = std::move(item);
  for (auto& item : *source.mutable_key_value_items()) {
    *target->add_key_value_items() = std::move(item);
  }
  for (auto& item : *source.mutable_form_items()) {
    *target->add_form_items() = std::move(item);
  }

  merge_root_group(std::move(*source.mutable_body()), target->mutable_body());
  merge_root_group(std::move(*source.mutable_furniture()), target->mutable_furniture());

  for (auto& page : *source.mutable_pages()) {
    target->mutable_pages()->emplace(page.first, std::move(page.second));
  }
  if (!target->has_origin() && source.has_origin()) {
    *target->mutable_origin() = std::move(*source.mutable_origin());
  }
}

}  // namespace grparse
