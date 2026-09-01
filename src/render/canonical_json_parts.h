// The leaf serializers of the canonical JSON dialect: the payload values,
// the shared scalar shapes every node embeds (references, spans, boxes,
// provenance, images, formatting), and the meta variants. The document walk
// and the per-item serializers sit on top of this in
// canonical_json_renderer.cpp; both halves write through the one writer this
// class owns. Internal to the export renderers;
// include/grparse/document_render.h stays the only public surface.
#ifndef GRPARSE_RENDER_CANONICAL_JSON_PARTS_H
#define GRPARSE_RENDER_CANONICAL_JSON_PARTS_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/struct.pb.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "canonical_json_labels.h"
#include "canonical_json_writer.h"
#include "renderer_base.h"

namespace grparse::render {

namespace docv1 = ai::pipestream::document::v1;

class CanonicalJsonParts {
 protected:
  using ValueMap =
      google::protobuf::Map<std::string, google::protobuf::Value>;

  JsonWriter writer_;

  // -- google.protobuf.Value payloads ---------------------------------------

  void emit_value(const google::protobuf::Value& value) {
    switch (value.kind_case()) {
      case google::protobuf::Value::kBoolValue:
        writer_.value_bool(value.bool_value());
        break;
      case google::protobuf::Value::kNumberValue: {
        const double number = value.number_value();
        // The dump narrows integral numbers to integers.
        if (std::isfinite(number) && std::floor(number) == number) {
          writer_.value_raw(canonical_integral_decimal(number));
        } else {
          writer_.value_raw(canonical_double(number));
        }
        break;
      }
      case google::protobuf::Value::kStringValue:
        writer_.value_string(value.string_value());
        break;
      case google::protobuf::Value::kStructValue:
        emit_struct(value.struct_value());
        break;
      case google::protobuf::Value::kListValue:
        writer_.begin_array();
        for (const auto& item : value.list_value().values()) emit_value(item);
        writer_.end_array();
        break;
      default:  // null and unset both render as null
        writer_.value_null();
        break;
    }
  }

  void emit_struct(const google::protobuf::Struct& payload) {
    writer_.begin_object();
    // Unordered wire map: emitted in byte order for a deterministic export
    // (nested nulls stay, unlike top-level custom fields).
    std::vector<std::string_view> keys;
    keys.reserve(payload.fields().size());
    for (const auto& [key, unused] : payload.fields()) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    for (const auto key : keys) {
      writer_.key(key);
      emit_value(payload.fields().at(std::string(key)));
    }
    writer_.end_object();
  }

  // Emits the map's entries as trailing members of the current object, in the
  // shared export order (renderer_base.h: pipestream-namespaced renaming of
  // non-conforming names, null payloads dropped, final names in byte order).
  void emit_custom_fields(const ValueMap& fields) {
    for (const auto& [key, value] : ordered_custom_fields(fields)) {
      writer_.key(key);
      emit_value(*value);
    }
  }

  // -- shared scalar shapes -------------------------------------------------

  void emit_ref_object(const docv1::RefItem& ref) {
    writer_.begin_object();
    writer_.member_string("$ref", ref.ref());
    writer_.end_object();
  }

  void emit_ref_array(
      const google::protobuf::RepeatedPtrField<docv1::RefItem>& refs) {
    writer_.begin_array();
    for (const auto& ref : refs) emit_ref_object(ref);
    writer_.end_array();
  }

  void emit_span(std::int64_t start, std::int64_t end) {
    writer_.begin_array();
    writer_.value_int(start);
    writer_.value_int(end);
    writer_.end_array();
  }

  void emit_bbox(const docv1::BoundingBox& bbox) {
    writer_.begin_object();
    writer_.member_double("l", bbox.l());
    writer_.member_double("t", bbox.t());
    writer_.member_double("r", bbox.r());
    writer_.member_double("b", bbox.b());
    writer_.member_string("coord_origin", coord_origin_string(bbox));
    writer_.end_object();
  }

  void emit_prov(const docv1::ProvenanceItem& prov) {
    writer_.begin_object();
    writer_.member_int("page_no", prov.page_no());
    writer_.key("bbox");
    emit_bbox(prov.bbox());
    writer_.key("charspan");
    emit_span(prov.charspan().start(), prov.charspan().end());
    writer_.end_object();
  }

  void emit_prov_list(
      const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& provs) {
    writer_.key("prov");
    writer_.begin_array();
    for (const auto& prov : provs) emit_prov(prov);
    writer_.end_array();
  }

  void emit_size(const docv1::Size& size) {
    writer_.begin_object();
    writer_.member_double("width", size.width());
    writer_.member_double("height", size.height());
    writer_.end_object();
  }

  void emit_image_ref(const docv1::ImageRef& image) {
    writer_.begin_object();
    writer_.member_string("mimetype", image.mimetype());
    writer_.member_int("dpi", image.dpi());
    writer_.key("size");
    emit_size(image.size());
    writer_.member_string("uri", normalized_uri(image.uri()));
    writer_.end_object();
  }

  // Emits the source list member when it carries representable entries.
  // Collector attribution entries and unset arms have no canonical form and
  // are dropped; a list left empty is suppressed entirely.
  void emit_source_list(
      const google::protobuf::RepeatedPtrField<docv1::SourceType>& sources) {
    std::vector<const docv1::TrackSource*> tracks;
    for (const auto& source : sources) {
      if (source.source_case() == docv1::SourceType::kTrack) {
        tracks.push_back(&source.track());
      }
    }
    if (tracks.empty()) return;
    writer_.key("source");
    writer_.begin_array();
    for (const auto* track : tracks) {
      writer_.begin_object();
      writer_.member_string("kind", "track");
      writer_.member_double("start_time", track->start_time());
      writer_.member_double("end_time", track->end_time());
      if (track->has_identifier()) writer_.member_string("identifier", track->identifier());
      if (track->has_voice()) writer_.member_string("voice", track->voice());
      writer_.end_object();
    }
    writer_.end_array();
  }

  // Emits the comments member unless empty (suppressed like source).
  void emit_comments(
      const google::protobuf::RepeatedPtrField<docv1::FineRef>& comments) {
    if (comments.empty()) return;
    writer_.key("comments");
    writer_.begin_array();
    for (const auto& fine : comments) {
      writer_.begin_object();
      writer_.member_string("$ref", fine.ref());
      if (fine.has_range()) {
        writer_.key("range");
        emit_span(fine.range().start(), fine.range().end());
      }
      writer_.end_object();
    }
    writer_.end_array();
  }

  void emit_formatting(const docv1::Formatting& formatting) {
    writer_.begin_object();
    writer_.member_bool("bold", formatting.bold());
    writer_.member_bool("italic", formatting.italic());
    writer_.member_bool("underline", formatting.underline());
    writer_.member_bool("strikethrough", formatting.strikethrough());
    writer_.member_string("script", script_string(formatting.script()));
    writer_.end_object();
  }

  // -- meta fields ----------------------------------------------------------

  // The {confidence, created_by} prediction prefix shared by every scored
  // meta field.
  template <typename Message>
  void emit_prediction_prefix(const Message& message) {
    if (message.has_confidence()) writer_.member_double("confidence", message.confidence());
    if (message.has_created_by()) writer_.member_string("created_by", message.created_by());
  }

  void emit_summary_meta(const docv1::SummaryMetaField& meta) {
    writer_.key("summary");
    writer_.begin_object();
    emit_prediction_prefix(meta);
    writer_.member_string("text", meta.text());
    emit_custom_fields(meta.custom_fields());
    writer_.end_object();
  }

  // Emits the language member only when the code is representable; a
  // raw-only language cannot be reconstructed and the whole field drops.
  void emit_language_meta(const docv1::LanguageMetaField& meta) {
    const auto code = human_language_string(meta.code());
    if (!code) return;
    writer_.key("language");
    writer_.begin_object();
    emit_prediction_prefix(meta);
    writer_.member_string("code", *code);
    emit_custom_fields(meta.custom_fields());
    writer_.end_object();
  }

  // Emits the entities member unless it has no mentions (dropped whole).
  void emit_entities_meta(const docv1::EntitiesMetaField& meta) {
    if (meta.mentions().empty()) return;
    writer_.key("entities");
    writer_.begin_object();
    writer_.key("mentions");
    writer_.begin_array();
    for (const auto& mention : meta.mentions()) {
      writer_.begin_object();
      emit_prediction_prefix(mention);
      writer_.member_string("text", mention.text());
      if (mention.has_orig()) writer_.member_string("orig", mention.orig());
      if (mention.has_label()) writer_.member_string("label", mention.label());
      if (mention.has_charspan()) {
        writer_.key("charspan");
        emit_span(mention.charspan().start(), mention.charspan().end());
      }
      emit_custom_fields(mention.custom_fields());
      writer_.end_object();
    }
    writer_.end_array();
    emit_custom_fields(meta.custom_fields());
    writer_.end_object();
  }

  // Emits a keywords/topics-shaped member unless its values are empty.
  template <typename Message>
  void emit_values_meta(std::string_view name, const Message& meta) {
    if (meta.values().empty()) return;
    writer_.key(name);
    writer_.begin_object();
    writer_.key("values");
    writer_.begin_array();
    // The model's list type is a unique list: a repeat drops on load, keeping
    // the first occurrence.
    std::set<std::string_view> seen;
    for (const auto& value : meta.values()) {
      if (seen.insert(value).second) writer_.value_string(value);
    }
    writer_.end_array();
    emit_custom_fields(meta.custom_fields());
    writer_.end_object();
  }

  // The five members every meta variant starts with.
  template <typename Message>
  void emit_inherited_meta_members(const Message& meta) {
    if (meta.has_summary()) emit_summary_meta(meta.summary());
    if (meta.has_language()) emit_language_meta(meta.language());
    if (meta.has_entities()) emit_entities_meta(meta.entities());
    if (meta.has_keywords()) emit_values_meta("keywords", meta.keywords());
    if (meta.has_topics()) emit_values_meta("topics", meta.topics());
  }

  void emit_base_meta(const docv1::BaseMeta& meta) {
    writer_.key("meta");
    writer_.begin_object();
    emit_inherited_meta_members(meta);
    emit_custom_fields(meta.custom_fields());
    writer_.end_object();
  }

  void emit_description_meta(const docv1::DescriptionMetaField& meta) {
    writer_.key("description");
    writer_.begin_object();
    emit_prediction_prefix(meta);
    writer_.member_string("text", meta.text());
    emit_custom_fields(meta.custom_fields());
    writer_.end_object();
  }

  void emit_floating_meta(const docv1::FloatingMeta& meta) {
    writer_.key("meta");
    writer_.begin_object();
    emit_inherited_meta_members(meta);
    if (meta.has_description()) emit_description_meta(meta.description());
    emit_custom_fields(meta.custom_fields());
    writer_.end_object();
  }

  // A base-shaped meta re-read as a floating one: the inherited members and
  // custom fields carry over; the floating-only members have no source.
  void emit_base_meta_as_floating(const docv1::BaseMeta& meta) {
    writer_.key("meta");
    writer_.begin_object();
    emit_inherited_meta_members(meta);
    emit_custom_fields(meta.custom_fields());
    writer_.end_object();
  }
};

}  // namespace grparse::render

#endif
