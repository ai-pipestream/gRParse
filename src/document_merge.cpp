#include "grparse/document_merge.h"

#include <map>
#include <optional>
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

const auto self_ref_field = [](const auto& item) { return item.self_ref(); };

// forward declaration for the recursion between the merge and the claim walk
struct Tracking;
void claim_fields_under(google::protobuf::Message* message, const Tracking& tracking);

// Provenance tracking for a message that carries a `field_sources` list:
// the list itself, the path prefix of the message inside the tracked root,
// and the collector claiming the source's answers.
struct Tracking {
  google::protobuf::Message* root = nullptr;
  const google::protobuf::FieldDescriptor* list = nullptr;
  std::string prefix;
  const docv1::CollectorSource* claimant = nullptr;
  std::string mimetype;
};

const google::protobuf::FieldDescriptor* field_sources_of(
    const google::protobuf::Descriptor* descriptor) {
  const auto* field = descriptor->FindFieldByName("field_sources");
  if (field == nullptr || !field->is_repeated() ||
      field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE ||
      field->message_type()->full_name() != docv1::FieldSource::descriptor()->full_name()) {
    return nullptr;
  }
  return field;
}

// The recorded source of `path` on the tracked root, when one is recorded.
docv1::FieldSource* recorded(const Tracking& tracking, const std::string& path) {
  const auto* reflection = tracking.root->GetReflection();
  const int size = reflection->FieldSize(*tracking.root, tracking.list);
  for (int index = 0; index < size; ++index) {
    auto* entry = static_cast<docv1::FieldSource*>(
        reflection->MutableRepeatedMessage(tracking.root, tracking.list, index));
    if (entry->field() == path) return entry;
  }
  return nullptr;
}

void record(const Tracking& tracking, const std::string& path) {
  docv1::FieldSource* entry = recorded(tracking, path);
  if (entry == nullptr) {
    entry = static_cast<docv1::FieldSource*>(
        tracking.root->GetReflection()->AddMessage(tracking.root, tracking.list));
    entry->set_field(path);
  }
  *entry->mutable_source() = *tracking.claimant;
}

// Whether the claimant's answer displaces the one the target carries:
// confidence first when both state one, then standing for the format; a
// tie leaves the target's answer.
bool claimant_wins(const Tracking& tracking, const std::string& path) {
  const docv1::FieldSource* holder = recorded(tracking, path);
  if (holder == nullptr) return false;
  const auto& incumbent = holder->source();
  const auto& challenger = *tracking.claimant;
  if (incumbent.has_confidence() && challenger.has_confidence() &&
      incumbent.confidence() != challenger.confidence()) {
    return challenger.confidence() > incumbent.confidence();
  }
  return document_claim_rank(challenger.collector(), tracking.mimetype) >
         document_claim_rank(incumbent.collector(), tracking.mimetype);
}

// The scatter-gather rule for everything that is not an arena, applied by
// reflection so a field the schema grows is carried the day it lands
// instead of the day someone notices it missing: a singular field the
// target has not answered takes the source's answer, a message answered by
// both merges field by field, a list appends, and a map keeps the target's
// entry for a key both carry. Nothing the target already says is changed,
// except under provenance tracking, where a claimant that outranks the
// recorded holder of a field displaces it and is recorded in its place.
void merge_message(google::protobuf::Message&& source, google::protobuf::Message* target,
                   Tracking tracking) {
  const auto* descriptor = source.GetDescriptor();
  const auto* from = source.GetReflection();
  const auto* to = target->GetReflection();
  if (tracking.claimant != nullptr) {
    if (const auto* list = field_sources_of(descriptor); list != nullptr) {
      // This message tracks its own fields; the claimant is the authority
      // on where the source's answers came from, so any list the source
      // carried is re-derived here rather than appended.
      from->ClearField(&source, list);
      tracking.root = target;
      tracking.list = list;
      tracking.prefix.clear();
    }
  }
  const bool tracked = tracking.claimant != nullptr && tracking.root != nullptr;
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
      Tracking nested = tracking;
      if (tracked) nested.prefix = tracking.prefix + std::string(field->name()) + ".";
      if (to->HasField(*target, field)) {
        merge_message(std::move(*from->MutableMessage(&source, field)),
                      to->MutableMessage(target, field), nested);
      } else {
        to->SwapFields(target, &source, {field});
        if (tracking.claimant != nullptr) {
          // A message taken whole: every singular field it answers is the
          // claimant's. One that tracks itself starts its own list here,
          // re-derived from the claimant rather than carried from the
          // source; one that does not is attributed under the enclosing
          // tracked message, when there is one.
          auto* taken = to->MutableMessage(target, field);
          if (const auto* list = field_sources_of(field->message_type()); list != nullptr) {
            taken->GetReflection()->ClearField(taken, list);
            Tracking own = tracking;
            own.root = taken;
            own.list = list;
            own.prefix.clear();
            claim_fields_under(taken, own);
          } else if (tracked) {
            claim_fields_under(taken, nested);
          }
        }
      }
      continue;
    }
    const std::string path = tracking.prefix + std::string(field->name());
    if (!to->HasField(*target, field)) {
      to->SwapFields(target, &source, {field});
      if (tracked) record(tracking, path);
    } else if (tracked && claimant_wins(tracking, path)) {
      to->SwapFields(target, &source, {field});
      record(tracking, path);
    }
  }
}

// Records the claimant for every singular field `message` answers, into
// the tracking root, recursing through nested messages.
void claim_fields_under(google::protobuf::Message* message, const Tracking& tracking) {
  const auto* descriptor = message->GetDescriptor();
  const auto* reflection = message->GetReflection();
  for (int index = 0; index < descriptor->field_count(); ++index) {
    const auto* field = descriptor->field(index);
    if (field->is_repeated() || !reflection->HasField(*message, field)) continue;
    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      Tracking nested = tracking;
      nested.prefix = tracking.prefix + std::string(field->name()) + ".";
      claim_fields_under(reflection->MutableMessage(message, field), nested);
      continue;
    }
    record(tracking, tracking.prefix + std::string(field->name()));
  }
}

// The document-level account a source carries, whole, for Document.claims.
// Absent when the source says nothing at the document level.
std::optional<docv1::CollectorClaim> claim_of(const docv1::Document& source,
                                              const docv1::CollectorSource& claimant) {
  if (!source.has_source_meta() && !source.has_origin() && source.page_styles_size() == 0 &&
      !source.has_email() && !source.has_media()) {
    return std::nullopt;
  }
  docv1::CollectorClaim claim;
  *claim.mutable_source() = claimant;
  if (source.has_source_meta()) *claim.mutable_source_meta() = source.source_meta();
  if (source.has_origin()) *claim.mutable_origin() = source.origin();
  for (const auto& style : source.page_styles()) *claim.add_page_styles() = style;
  if (source.has_email()) *claim.mutable_email() = source.email();
  if (source.has_media()) *claim.mutable_media() = source.media();
  // A claim's own account never carries provenance lists: it is one
  // collector's word, whole.
  claim.mutable_source_meta()->clear_field_sources();
  claim.mutable_origin()->clear_field_sources();
  if (!source.has_source_meta()) claim.clear_source_meta();
  if (!source.has_origin()) claim.clear_origin();
  return claim;
}

void merge_arenas(docv1::Document&& source, docv1::Document* target) {
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
}

}  // namespace

void rewrite_references(const std::map<std::string, std::string>& renumbering,
                        google::protobuf::Message* message) {
  rewrite_refs(renumbering, message);
}

void merge_documents(docv1::Document&& source, docv1::Document* target) {
  merge_arenas(std::move(source), target);
  merge_message(std::move(source), target, Tracking{});
}

void merge_documents(docv1::Document&& source, docv1::Document* target,
                     const docv1::CollectorSource& claimant) {
  if (claimant.collector().empty()) {
    merge_documents(std::move(source), target);
    return;
  }
  std::optional<docv1::CollectorClaim> claim = claim_of(source, claimant);
  merge_arenas(std::move(source), target);
  Tracking tracking;
  tracking.claimant = &claimant;
  tracking.mimetype = target->origin().mimetype();
  merge_message(std::move(source), target, tracking);
  if (claim.has_value()) *target->add_claims() = std::move(*claim);
}

int document_claim_rank(const std::string& collector, const std::string& mimetype) {
  if (collector == "grparse") return 100;
  const bool ooxml = mimetype.contains("officedocument");
  const bool ole2 = mimetype == "application/msword" || mimetype.contains("ms-excel") ||
                    mimetype.contains("ms-powerpoint");
  const bool opendocument = mimetype.contains("opendocument");
  const bool spreadsheet = mimetype.contains("spreadsheet") || mimetype.contains("ms-excel") ||
                           mimetype == "text/csv";
  const bool office = ooxml || ole2 || opendocument || mimetype == "text/csv" ||
                      mimetype == "application/rtf";
  if (collector == "poi") return ooxml || ole2 ? 3 : 0;
  if (collector == "calamine") return spreadsheet ? 2 : 0;
  if (collector == "libreoffice") return office ? 1 : 0;
  return 0;
}

void claim_fields(google::protobuf::Message* tracked, const docv1::CollectorSource& claimant) {
  const auto* list = field_sources_of(tracked->GetDescriptor());
  if (list == nullptr) return;
  Tracking tracking;
  tracking.root = tracked;
  tracking.list = list;
  tracking.claimant = &claimant;
  claim_fields_under(tracked, tracking);
}

}  // namespace grparse
