#include "grparse/document_merge.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

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
      if (const auto mapped = mapping.find(reflection->GetString(*message, field));
          mapped != mapping.end()) {
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

// The key of one map entry as a string, for telling whether the target
// already answers it. Map keys are integral, boolean, or string.
std::string map_key_repr(const google::protobuf::Message& entry) {
  const auto* field = entry.GetDescriptor()->map_key();
  const auto* reflection = entry.GetReflection();
  using google::protobuf::FieldDescriptor;
  switch (field->cpp_type()) {
    case FieldDescriptor::CPPTYPE_INT32: return std::to_string(reflection->GetInt32(entry, field));
    case FieldDescriptor::CPPTYPE_INT64: return std::to_string(reflection->GetInt64(entry, field));
    case FieldDescriptor::CPPTYPE_UINT32: return std::to_string(reflection->GetUInt32(entry, field));
    case FieldDescriptor::CPPTYPE_UINT64: return std::to_string(reflection->GetUInt64(entry, field));
    case FieldDescriptor::CPPTYPE_BOOL: return reflection->GetBool(entry, field) ? "1" : "0";
    case FieldDescriptor::CPPTYPE_STRING: return reflection->GetString(entry, field);
    default: return std::string();
  }
}

// Appends one repeated scalar element of `source` to `target`.
void append_scalar(const google::protobuf::Message& source, google::protobuf::Message* target,
                   const google::protobuf::FieldDescriptor* field, int index) {
  const auto* from = source.GetReflection();
  const auto* to = target->GetReflection();
  using google::protobuf::FieldDescriptor;
  switch (field->cpp_type()) {
    case FieldDescriptor::CPPTYPE_INT32:
      to->AddInt32(target, field, from->GetRepeatedInt32(source, field, index));
      break;
    case FieldDescriptor::CPPTYPE_INT64:
      to->AddInt64(target, field, from->GetRepeatedInt64(source, field, index));
      break;
    case FieldDescriptor::CPPTYPE_UINT32:
      to->AddUInt32(target, field, from->GetRepeatedUInt32(source, field, index));
      break;
    case FieldDescriptor::CPPTYPE_UINT64:
      to->AddUInt64(target, field, from->GetRepeatedUInt64(source, field, index));
      break;
    case FieldDescriptor::CPPTYPE_DOUBLE:
      to->AddDouble(target, field, from->GetRepeatedDouble(source, field, index));
      break;
    case FieldDescriptor::CPPTYPE_FLOAT:
      to->AddFloat(target, field, from->GetRepeatedFloat(source, field, index));
      break;
    case FieldDescriptor::CPPTYPE_BOOL:
      to->AddBool(target, field, from->GetRepeatedBool(source, field, index));
      break;
    case FieldDescriptor::CPPTYPE_ENUM:
      to->AddEnumValue(target, field, from->GetRepeatedEnumValue(source, field, index));
      break;
    case FieldDescriptor::CPPTYPE_STRING:
      to->AddString(target, field, from->GetRepeatedString(source, field, index));
      break;
    case FieldDescriptor::CPPTYPE_MESSAGE:
      break;
  }
}

// The scatter-gather rule for everything that is not an arena, applied by
// reflection so a field the schema grows is carried the day it lands
// instead of the day someone notices it missing: a singular field the
// target has not answered takes the source's answer, a message answered by
// both merges field by field, a list appends, and a map keeps the target's
// entry for a key both carry. Nothing the target already says is changed.
void merge_message(google::protobuf::Message&& source, google::protobuf::Message* target) {
  const auto* descriptor = source.GetDescriptor();
  const auto* from = source.GetReflection();
  const auto* to = target->GetReflection();
  for (int index = 0; index < descriptor->field_count(); ++index) {
    const auto* field = descriptor->field(index);
    if (field->is_map()) {
      std::map<std::string, bool> answered;
      const int existing = to->FieldSize(*target, field);
      for (int entry = 0; entry < existing; ++entry) {
        answered.emplace(map_key_repr(to->GetRepeatedMessage(*target, field, entry)), true);
      }
      const int incoming = from->FieldSize(source, field);
      for (int entry = 0; entry < incoming; ++entry) {
        auto* moved = from->MutableRepeatedMessage(&source, field, entry);
        if (answered.contains(map_key_repr(*moved))) continue;
        to->AddMessage(target, field)->CopyFrom(*moved);
      }
      continue;
    }
    if (field->is_repeated()) {
      const int incoming = from->FieldSize(source, field);
      if (incoming == 0) continue;
      if (to->FieldSize(*target, field) == 0) {
        to->SwapFields(target, &source, {field});
        continue;
      }
      for (int entry = 0; entry < incoming; ++entry) {
        if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
          to->AddMessage(target, field)->CopyFrom(from->GetRepeatedMessage(source, field, entry));
        } else {
          append_scalar(source, target, field, entry);
        }
      }
      continue;
    }
    if (!from->HasField(source, field)) continue;
    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      if (to->HasField(*target, field)) {
        merge_message(std::move(*from->MutableMessage(&source, field)),
                      to->MutableMessage(target, field));
      } else {
        to->SwapFields(target, &source, {field});
      }
      continue;
    }
    if (!to->HasField(*target, field)) to->SwapFields(target, &source, {field});
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
  map_arena(source.field_regions(), target->field_regions_size(), "#/field_regions/",
            self_ref_field, &mapping);
  map_arena(source.field_items(), target->field_items_size(), "#/field_items/",
            self_ref_field, &mapping);

  rewrite_refs(mapping, &source);

  // The arenas append in their renumbered order; the root groups' own
  // self_refs are fixed names and stay the target's.
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
  for (auto& item : *source.mutable_field_regions()) {
    *target->add_field_regions() = std::move(item);
  }
  for (auto& item : *source.mutable_field_items()) {
    *target->add_field_items() = std::move(item);
  }
  source.clear_groups();
  source.clear_texts();
  source.clear_pictures();
  source.clear_tables();
  source.clear_key_value_items();
  source.clear_form_items();
  source.clear_field_regions();
  source.clear_field_items();

  // Everything else: the root groups (children append, meta merges), pages
  // by number with the target winning a collision, the origin and the
  // source metadata field by field beside whatever the service stamped, and
  // every document-level carrier the model has or grows.
  merge_message(std::move(source), target);
}

}  // namespace grparse
