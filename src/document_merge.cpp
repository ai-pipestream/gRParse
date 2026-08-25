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

// Appends the source root group's children and metadata to the target's.
// Children arrive already rewritten.
void merge_root_group(docv1::GroupItem&& source, docv1::GroupItem* target) {
  for (auto& child : *source.mutable_children()) {
    *target->add_children() = std::move(child);
  }
  if (source.has_meta()) {
    auto& fields = *target->mutable_meta()->mutable_custom_fields();
    for (auto& [key, value] : *source.mutable_meta()->mutable_custom_fields()) {
      // Additive: an existing key wins, a new key lands.
      fields.emplace(key, std::move(value));
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
  map_arena(source.field_regions(), target->field_regions_size(), "#/field_regions/",
            self_ref_field, &mapping);
  map_arena(source.field_items(), target->field_items_size(), "#/field_items/",
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
  for (auto& item : *source.mutable_field_regions()) {
    *target->add_field_regions() = std::move(item);
  }
  for (auto& item : *source.mutable_field_items()) {
    *target->add_field_items() = std::move(item);
  }

  merge_root_group(std::move(*source.mutable_body()), target->mutable_body());
  merge_root_group(std::move(*source.mutable_furniture()), target->mutable_furniture());

  for (auto& [number, page] : *source.mutable_pages()) {
    target->mutable_pages()->emplace(number, std::move(page));
  }
  // The origin merges field by field rather than whole. The service stamps
  // the archive's own filename, mimetype, and hash before any collector
  // runs, so a wholesale move would only ever land when nothing had been
  // stamped, and the web provenance a collector discovers inside the payload
  // (the crawled URL, the page's canonical URI) would be dropped every time.
  // The scatter-gather rule still holds: a field the target already carries
  // is never overwritten, so the archive leg and the HTML leg both survive.
  if (source.has_origin()) {
    docv1::DocumentOrigin* origin = target->mutable_origin();
    const docv1::DocumentOrigin& incoming = source.origin();
    if (origin->mimetype().empty()) origin->set_mimetype(incoming.mimetype());
    if (origin->filename().empty()) origin->set_filename(incoming.filename());
    if (origin->binary_hash() == 0) origin->set_binary_hash(incoming.binary_hash());
    if (!origin->has_uri() && incoming.has_uri()) origin->set_uri(incoming.uri());
    if (incoming.has_web()) {
      docv1::WebMeta* web = origin->mutable_web();
      const docv1::WebMeta& source_web = incoming.web();
      if (!web->has_target_uri() && source_web.has_target_uri()) {
        web->set_target_uri(source_web.target_uri());
      }
      if (!web->has_canonical_uri() && source_web.has_canonical_uri()) {
        web->set_canonical_uri(source_web.canonical_uri());
      }
      if (!web->has_crawl_time() && source_web.has_crawl_time()) {
        *web->mutable_crawl_time() = source_web.crawl_time();
      }
      if (!web->has_crawl_time_raw() && source_web.has_crawl_time_raw()) {
        web->set_crawl_time_raw(source_web.crawl_time_raw());
      }
      if (!web->has_http_status() && source_web.has_http_status()) {
        web->set_http_status(source_web.http_status());
      }
      if (!web->has_content_language() && source_web.has_content_language()) {
        web->set_content_language(source_web.content_language());
      }
      // emplace leaves a name the target already answered alone.
      for (const auto& [name, value] : source_web.headers()) {
        web->mutable_headers()->emplace(name, value);
      }
    }
  }
  // Source-declared metadata merges the same way; the page-level pairs
  // append, because two collectors reading the same page report different
  // tags rather than competing answers to one.
  if (source.has_source_meta()) {
    docv1::DocumentMeta* meta = target->mutable_source_meta();
    const docv1::DocumentMeta& incoming = source.source_meta();
    if (!meta->has_title() && incoming.has_title()) meta->set_title(incoming.title());
    if (!meta->has_created() && incoming.has_created()) {
      *meta->mutable_created() = incoming.created();
    }
    if (!meta->has_modified() && incoming.has_modified()) {
      *meta->mutable_modified() = incoming.modified();
    }
    if (!meta->has_created_raw() && incoming.has_created_raw()) {
      meta->set_created_raw(incoming.created_raw());
    }
    if (!meta->has_modified_raw() && incoming.has_modified_raw()) {
      meta->set_modified_raw(incoming.modified_raw());
    }
    if (!meta->has_language() && incoming.has_language()) {
      meta->set_language(incoming.language());
    }
    if (!meta->has_generator() && incoming.has_generator()) {
      meta->set_generator(incoming.generator());
    }
    for (const auto& author : incoming.authors()) meta->add_authors(author);
    for (const auto& keyword : incoming.keywords()) meta->add_keywords(keyword);
    for (const auto& [name, value] : incoming.extra()) {
      meta->mutable_extra()->emplace(name, value);
    }
  }
  // The remaining model-extension carriers: list-shaped ones append,
  // singular ones keep the first collector's claim exactly like origin.
  for (auto& tag : *source.mutable_meta_tags()) {
    *target->add_meta_tags() = std::move(tag);
  }
  for (auto& item : *source.mutable_attachments()) *target->add_attachments() = std::move(item);
  for (auto& item : *source.mutable_outline()) *target->add_outline() = std::move(item);
  for (auto& item : *source.mutable_structured_data()) {
    *target->add_structured_data() = std::move(item);
  }
  if (!target->has_media() && source.has_media()) {
    *target->mutable_media() = std::move(*source.mutable_media());
  }
}

}  // namespace grparse
