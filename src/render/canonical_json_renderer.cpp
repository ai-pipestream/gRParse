// Renders the document in the upstream-canonical JSON dialect. The walk
// mirrors the reference schema bindings' proto-to-model import followed by
// the model's canonical dump (two-space indent, ASCII-only strings,
// model-declaration key order, exclude-none semantics), so the output is
// byte-compatible with that pipeline without leaving C++.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/struct.pb.h>

#include "grparse/document_render.h"
#include "grparse/schema_version.h"
#include "canonical_json_labels.h"
#include "canonical_json_parts.h"
#include "canonical_json_writer.h"
#include "load_normalization.h"
#include "renderer_base.h"

namespace grparse {
namespace {

namespace docv1 = ai::pipestream::document::v1;
using render::code_language_string;
using render::content_layer_string;
using render::derived_table_grid;
using render::doc_item_label_string;
using render::exported_code_language;
using render::graph_cell_label_string;
using render::graph_link_label_string;
using render::group_label_string;
using render::normalized_uri;
using render::orientation_string;

// The canonical dialect identity. The wire schema_name/version are
// service-internal and deliberately not echoed: the export always declares
// the dialect's own schema name and the schema version this renderer
// implements.
constexpr std::string_view kSchemaName = "DoclingDocument";
constexpr std::string_view kSchemaVersion = kUpstreamSchemaVersion;

// ---------------------------------------------------------------------------
// The renderer.
// ---------------------------------------------------------------------------

class CanonicalJsonRenderer : public render::CanonicalJsonParts {
 public:
  std::string render(const docv1::Document& document) {
    // The JSON is a constant factor larger than the wire form (field names
    // and indentation on top of 1:1 payloads); reserving up front keeps the
    // multi-megabyte page-image URIs from forcing repeated buffer growth.
    const std::size_t wire_bytes = document.ByteSizeLong();
    writer_.reserve(wire_bytes + wire_bytes / 4 + 4096);
    emit_document(document);
    return writer_.take();
  }

 private:
  // The typed barcode annotations project into the dialect as one
  // namespaced custom field; the wire itself never carries the untyped
  // copy. The reference-side importer synthesizes the identical entry, so
  // both exporters stay byte-equal.
  static ValueMap picture_custom_fields(const docv1::PictureItem& picture) {
    ValueMap merged;
    if (picture.has_meta()) merged = picture.meta().custom_fields();
    google::protobuf::ListValue payloads;
    for (const auto& annotation : picture.annotations()) {
      if (!annotation.has_barcode()) continue;
      auto& entry = *payloads.add_values()->mutable_struct_value()->mutable_fields();
      entry["format"].set_string_value(annotation.barcode().format());
      entry["provenance"].set_string_value(annotation.barcode().provenance());
      entry["value"].set_string_value(annotation.barcode().value());
    }
    if (payloads.values_size() > 0 && merged.count("pipestream__barcodes") == 0) {
      *merged["pipestream__barcodes"].mutable_list_value() = std::move(payloads);
    }
    return merged;
  }

  void emit_picture_meta(const docv1::PictureItem& picture) {
    const ValueMap custom = picture_custom_fields(picture);
    if (!picture.has_meta() && custom.empty()) return;
    static const docv1::PictureMeta kEmptyMeta;
    const docv1::PictureMeta& meta = picture.has_meta() ? picture.meta() : kEmptyMeta;
    writer_.key("meta");
    writer_.begin_object();
    emit_inherited_meta_members(meta);
    if (meta.has_description()) emit_description_meta(meta.description());
    if (meta.has_classification()) {
      writer_.key("classification");
      writer_.begin_object();
      writer_.key("predictions");
      writer_.begin_array();
      for (const auto& prediction : meta.classification().predictions()) {
        writer_.begin_object();
        emit_prediction_prefix(prediction);
        writer_.member_string("class_name", prediction.class_name());
        emit_custom_fields(prediction.custom_fields());
        writer_.end_object();
      }
      writer_.end_array();
      emit_custom_fields(meta.classification().custom_fields());
      writer_.end_object();
    }
    if (meta.has_molecule()) {
      writer_.key("molecule");
      writer_.begin_object();
      emit_prediction_prefix(meta.molecule());
      writer_.member_string("smi", meta.molecule().smi());
      emit_custom_fields(meta.molecule().custom_fields());
      writer_.end_object();
    }
    if (meta.has_tabular_chart()) {
      const auto& chart = meta.tabular_chart();
      writer_.key("tabular_chart");
      writer_.begin_object();
      emit_prediction_prefix(chart);
      if (chart.has_title()) writer_.member_string("title", chart.title());
      writer_.key("chart_data");
      emit_table_data(chart.chart_data());
      emit_custom_fields(chart.custom_fields());
      writer_.end_object();
    }
    if (meta.has_code()) {
      const auto& code = meta.code();
      writer_.key("code");
      writer_.begin_object();
      emit_prediction_prefix(code);
      writer_.member_string("text", code.text());
      // A recognized tag maps; an unrecognized state with a raw fallback
      // collapses to the catch-all; anything else leaves the optional
      // language out entirely.
      const auto language = code_language_string(code.language());
      if (language || !code.language_raw().empty()) {
        writer_.member_string("language", language.value_or("unknown"));
      }
      emit_custom_fields(code.custom_fields());
      writer_.end_object();
    }
    emit_custom_fields(custom);
    writer_.end_object();
  }

  // -- table shapes ---------------------------------------------------------

  void emit_table_cell(const docv1::TableCell& cell, bool include_ref) {
    writer_.begin_object();
    if (cell.has_bbox()) {
      writer_.key("bbox");
      emit_bbox(cell.bbox());
    }
    writer_.member_int("row_span", cell.row_span());
    writer_.member_int("col_span", cell.col_span());
    writer_.member_int("start_row_offset_idx", cell.start_row_offset_idx());
    writer_.member_int("end_row_offset_idx", cell.end_row_offset_idx());
    writer_.member_int("start_col_offset_idx", cell.start_col_offset_idx());
    writer_.member_int("end_col_offset_idx", cell.end_col_offset_idx());
    writer_.member_string("text", cell.text());
    writer_.member_bool("column_header", cell.column_header());
    writer_.member_bool("row_header", cell.row_header());
    writer_.member_bool("row_section", cell.row_section());
    writer_.member_bool("fillable", cell.fillable());
    if (include_ref && cell.has_ref()) {
      writer_.key("ref");
      emit_ref_object(cell.ref());
    }
    writer_.end_object();
  }

  // The default cell filling grid positions no table cell covers.
  void emit_grid_filler_cell(int row, int column) {
    writer_.begin_object();
    writer_.member_int("row_span", 1);
    writer_.member_int("col_span", 1);
    writer_.member_int("start_row_offset_idx", row);
    writer_.member_int("end_row_offset_idx", row + 1);
    writer_.member_int("start_col_offset_idx", column);
    writer_.member_int("end_col_offset_idx", column + 1);
    writer_.member_string("text", "");
    writer_.member_bool("column_header", false);
    writer_.member_bool("row_header", false);
    writer_.member_bool("row_section", false);
    writer_.member_bool("fillable", false);
    writer_.end_object();
  }

  void emit_table_data(const docv1::TableData& data) {
    writer_.begin_object();
    writer_.key("table_cells");
    writer_.begin_array();
    for (const auto& cell : data.table_cells()) emit_table_cell(cell, true);
    writer_.end_array();
    writer_.member_int("num_rows", data.num_rows());
    writer_.member_int("num_cols", data.num_cols());
    writer_.member_string("orientation", orientation_string(data.orientation()));

    // The grid is computed from the flat cell list and the declared
    // dimensions (renderer_base.h); the wire grid is a redundant projection
    // and is ignored. Spanned cells repeat at every position they cover, and
    // grid entries render as plain cells (no ref, matching the computed
    // field's type).
    const auto grid = derived_table_grid(data);
    writer_.key("grid");
    writer_.begin_array();
    for (std::size_t row = 0; row < grid.size(); ++row) {
      writer_.begin_array();
      for (std::size_t column = 0; column < grid[row].size(); ++column) {
        if (const auto* cell = grid[row][column]) {
          emit_table_cell(*cell, false);
        } else {
          emit_grid_filler_cell(static_cast<int>(row), static_cast<int>(column));
        }
      }
      writer_.end_array();
    }
    writer_.end_array();
    writer_.end_object();
  }

  // -- item prefixes --------------------------------------------------------

  // The node members every item starts with: self_ref, parent, children,
  // content_layer.
  template <typename Message>
  void emit_node_prefix(const Message& item) {
    writer_.member_string("self_ref", item.self_ref());
    if (item.has_parent()) {
      writer_.key("parent");
      emit_ref_object(item.parent());
    }
    writer_.key("children");
    emit_ref_array(item.children());
    writer_.member_string("content_layer", content_layer_string(item.content_layer()));
  }

  // The label member: the mapped tag when the item class accepts it, else
  // the class default (a raw-only label cannot be carried).
  void emit_label(docv1::DocItemLabel tag,
                  std::initializer_list<std::string_view> allowed,
                  std::string_view fallback) {
    const auto mapped = doc_item_label_string(tag);
    if (mapped &&
        std::find(allowed.begin(), allowed.end(), *mapped) != allowed.end()) {
      writer_.member_string("label", *mapped);
    } else {
      writer_.member_string("label", fallback);
    }
  }

  // -- text items -----------------------------------------------------------

  // Emits one flattened text item from its shared base. `label` is the
  // discriminator the arm (or the label-dispatch of the generic arm)
  // selected; `extras` appends the subclass members after hyperlink.
  template <typename ExtrasFn>
  void emit_text_from_base(const docv1::TextItemBase& base, std::string_view label,
                           ExtrasFn&& extras) {
    writer_.begin_object();
    emit_node_prefix(base);
    if (base.has_meta()) emit_base_meta(base.meta());
    writer_.member_string("label", label);
    emit_prov_list(base.prov());
    emit_source_list(base.source());
    emit_comments(base.comments());
    writer_.member_string("orig", base.orig());
    writer_.member_string("text", base.text());
    if (base.has_formatting()) {
      writer_.key("formatting");
      emit_formatting(base.formatting());
    }
    if (base.has_hyperlink()) {
      writer_.member_string("hyperlink", normalized_uri(base.hyperlink()));
    }
    extras();
    writer_.end_object();
  }

  // The code item shape shared by the dedicated arm and the generic arm's
  // label dispatch. All fields come from the CodeItem message directly.
  void emit_code_item(const docv1::CodeItem& code) {
    writer_.begin_object();
    emit_node_prefix(code);
    if (code.has_meta()) emit_floating_meta(code.meta());
    writer_.member_string("label", "code");
    emit_prov_list(code.prov());
    emit_source_list(code.source());
    emit_comments(code.comments());
    writer_.member_string("orig", code.orig());
    writer_.member_string("text", code.text());
    if (code.has_formatting()) {
      writer_.key("formatting");
      emit_formatting(code.formatting());
    }
    if (code.has_hyperlink()) {
      writer_.member_string("hyperlink", normalized_uri(code.hyperlink()));
    }
    writer_.key("captions");
    emit_ref_array(code.captions());
    writer_.key("references");
    emit_ref_array(code.references());
    writer_.key("footnotes");
    emit_ref_array(code.footnotes());
    if (code.has_image()) {
      writer_.key("image");
      emit_image_ref(code.image());
    }
    writer_.member_string("code_language", exported_code_language(code.code_language()));
    writer_.end_object();
  }

  // The generic text arm carrying a code label: the base fields flatten into
  // the code shape with defaults for every floating-only member, and the
  // base meta carries over as a floating meta.
  void emit_code_item_from_base(const docv1::TextItemBase& base) {
    writer_.begin_object();
    emit_node_prefix(base);
    if (base.has_meta()) emit_base_meta_as_floating(base.meta());
    writer_.member_string("label", "code");
    emit_prov_list(base.prov());
    emit_source_list(base.source());
    emit_comments(base.comments());
    writer_.member_string("orig", base.orig());
    writer_.member_string("text", base.text());
    if (base.has_formatting()) {
      writer_.key("formatting");
      emit_formatting(base.formatting());
    }
    if (base.has_hyperlink()) {
      writer_.member_string("hyperlink", normalized_uri(base.hyperlink()));
    }
    writer_.key("captions");
    emit_ref_array({});
    writer_.key("references");
    emit_ref_array({});
    writer_.key("footnotes");
    emit_ref_array({});
    writer_.member_string("code_language", "unknown");
    writer_.end_object();
  }

  void emit_level_member(std::int32_t level) {
    // A wire level of 0 is "unset"; the model default is 1.
    writer_.member_int("level", level != 0 ? level : 1);
  }

  void emit_list_item_members(bool enumerated, bool has_marker,
                              const std::string& marker) {
    writer_.member_bool("enumerated", enumerated);
    writer_.member_string("marker", has_marker ? marker : "-");
  }

  // The set of labels the generic text class accepts on the wire.
  static bool generic_text_label(std::string_view label) {
    static constexpr std::string_view kAllowed[] = {
        "caption",     "checkbox_selected", "checkbox_unselected",
        "footnote",    "page_footer",       "page_header",
        "paragraph",   "reference",         "text",
        "empty_value", "field_key",         "field_hint",
        "marker",      "handwritten_text",
    };
    return std::find(std::begin(kAllowed), std::end(kAllowed), label) !=
           std::end(kAllowed);
  }

  void emit_text_variant(const docv1::BaseTextItem& item) {
    switch (item.item_case()) {
      case docv1::BaseTextItem::kTitle:
        emit_text_from_base(item.title().base(), "title", [] {});
        return;
      case docv1::BaseTextItem::kSectionHeader:
        emit_text_from_base(item.section_header().base(), "section_header",
                            [&] { emit_level_member(item.section_header().level()); });
        return;
      case docv1::BaseTextItem::kFieldHeading:
        emit_text_from_base(item.field_heading().base(), "field_heading",
                            [&] { emit_level_member(item.field_heading().level()); });
        return;
      case docv1::BaseTextItem::kFieldValue:
        emit_text_from_base(item.field_value().base(), "field_value", [&] {
          const std::string& kind = item.field_value().kind();
          writer_.member_string("kind", !kind.empty() ? kind : "read_only");
        });
        return;
      case docv1::BaseTextItem::kListItem: {
        const auto& list_item = item.list_item();
        emit_text_from_base(list_item.base(), "list_item", [&] {
          emit_list_item_members(list_item.enumerated(), list_item.has_marker(),
                                 list_item.marker());
        });
        return;
      }
      case docv1::BaseTextItem::kCode:
        emit_code_item(item.code());
        return;
      case docv1::BaseTextItem::kFormula:
        emit_text_from_base(item.formula().base(), "formula", [] {});
        return;
      case docv1::BaseTextItem::kText: {
        // The generic arm discriminates by label alone: subclass labels
        // reconstruct their class with default subclass members.
        const auto& base = item.text().base();
        const auto mapped = doc_item_label_string(base.label());
        const std::string_view label = mapped.value_or("text");
        if (label == "code") {
          emit_code_item_from_base(base);
        } else if (label == "title" || label == "formula") {
          emit_text_from_base(base, label, [] {});
        } else if (label == "section_header" || label == "field_heading") {
          emit_text_from_base(base, label, [&] { emit_level_member(0); });
        } else if (label == "list_item") {
          emit_text_from_base(base, label,
                              [&] { emit_list_item_members(false, false, {}); });
        } else if (label == "field_value") {
          emit_text_from_base(base, label,
                              [&] { writer_.member_string("kind", "read_only"); });
        } else {
          emit_text_from_base(base, generic_text_label(label) ? label : "text",
                              [] {});
        }
        return;
      }
      default:
        // Dropping an unset arm would shift arena indices and corrupt every
        // subsequent reference; refuse instead.
        throw std::runtime_error(
            "canonical json: texts entry with unset item variant");
    }
  }

  // -- floating items -------------------------------------------------------

  // The caption/reference/footnote/image run shared by all floating items.
  template <typename Message>
  void emit_floating_suffix(const Message& item) {
    writer_.key("captions");
    emit_ref_array(item.captions());
    writer_.key("references");
    emit_ref_array(item.references());
    writer_.key("footnotes");
    emit_ref_array(item.footnotes());
    if (item.has_image()) {
      writer_.key("image");
      emit_image_ref(item.image());
    }
  }

  void emit_picture(const docv1::PictureItem& picture) {
    writer_.begin_object();
    emit_node_prefix(picture);
    emit_picture_meta(picture);
    emit_label(picture.label(), {"picture", "chart"}, "picture");
    emit_prov_list(picture.prov());
    emit_source_list(picture.source());
    emit_comments(picture.comments());
    emit_floating_suffix(picture);
    // The wire annotation list is a projection of meta and is ignored on
    // import; the dump always shows the (deprecated) empty list.
    writer_.key("annotations");
    writer_.begin_array();
    writer_.end_array();
    writer_.end_object();
  }

  void emit_table(const docv1::TableItem& table) {
    writer_.begin_object();
    emit_node_prefix(table);
    if (table.has_meta()) emit_floating_meta(table.meta());
    emit_label(table.label(), {"document_index", "table"}, "table");
    emit_prov_list(table.prov());
    emit_source_list(table.source());
    emit_comments(table.comments());
    emit_floating_suffix(table);
    writer_.key("data");
    emit_table_data(table.data());
    writer_.key("annotations");
    writer_.begin_array();
    writer_.end_array();
    writer_.end_object();
  }

  void emit_graph(const docv1::GraphData& graph) {
    writer_.begin_object();
    writer_.key("cells");
    writer_.begin_array();
    for (const auto& cell : graph.cells()) {
      writer_.begin_object();
      writer_.member_string("label", graph_cell_label_string(cell.label()));
      writer_.member_int("cell_id", cell.cell_id());
      writer_.member_string("text", cell.text());
      writer_.member_string("orig", cell.orig());
      if (cell.has_prov()) {
        writer_.key("prov");
        emit_prov(cell.prov());
      }
      if (cell.has_item_ref()) {
        writer_.key("item_ref");
        emit_ref_object(cell.item_ref());
      }
      writer_.end_object();
    }
    writer_.end_array();
    writer_.key("links");
    writer_.begin_array();
    for (const auto& link : graph.links()) {
      writer_.begin_object();
      writer_.member_string("label", graph_link_label_string(link.label()));
      writer_.member_int("source_cell_id", link.source_cell_id());
      writer_.member_int("target_cell_id", link.target_cell_id());
      writer_.end_object();
    }
    writer_.end_array();
    writer_.end_object();
  }

  template <typename Message>
  void emit_graph_item(const Message& item, std::string_view label) {
    writer_.begin_object();
    emit_node_prefix(item);
    if (item.has_meta()) emit_floating_meta(item.meta());
    writer_.member_string("label", label);
    emit_prov_list(item.prov());
    emit_source_list(item.source());
    emit_comments(item.comments());
    emit_floating_suffix(item);
    writer_.key("graph");
    emit_graph(item.graph());
    writer_.end_object();
  }

  template <typename Message>
  void emit_field_container_item(const Message& item, std::string_view label) {
    writer_.begin_object();
    emit_node_prefix(item);
    if (item.has_meta()) emit_base_meta(item.meta());
    writer_.member_string("label", label);
    emit_prov_list(item.prov());
    emit_source_list(item.source());
    emit_comments(item.comments());
    writer_.end_object();
  }

  // -- groups, pages, origin ------------------------------------------------

  void emit_group(const docv1::GroupItem& group) {
    writer_.begin_object();
    emit_node_prefix(group);
    if (group.has_meta()) emit_base_meta(group.meta());
    writer_.member_string("name", group.has_name() ? group.name() : "group");
    writer_.member_string("label", group_label_string(group.label()));
    writer_.end_object();
  }

  void emit_page(const docv1::PageItem& page) {
    writer_.begin_object();
    writer_.key("size");
    emit_size(page.size());
    if (page.has_image()) {
      writer_.key("image");
      emit_image_ref(page.image());
    }
    writer_.member_int("page_no", page.page_no());
    writer_.end_object();
  }

  void emit_origin(const docv1::DocumentOrigin& origin) {
    writer_.key("origin");
    writer_.begin_object();
    writer_.member_string("mimetype", origin.mimetype());
    writer_.key("binary_hash");
    writer_.value_uint(origin.binary_hash());
    writer_.member_string("filename", origin.filename());
    if (origin.has_uri()) writer_.member_string("uri", normalized_uri(origin.uri()));
    writer_.end_object();
  }

  // One arena member: the key, then every entry through `emit`.
  template <typename Items, typename Emit>
  void emit_arena(std::string_view name, const Items& items, Emit emit) {
    writer_.key(name);
    writer_.begin_array();
    for (const auto& item : items) emit(item);
    writer_.end_array();
  }

  // Integer-keyed pages dump under their decimal keys in ascending numeric
  // order.
  void emit_pages(const docv1::Document& document) {
    writer_.key("pages");
    writer_.begin_object();
    std::vector<std::int32_t> page_numbers;
    page_numbers.reserve(document.pages().size());
    for (const auto& [number, unused] : document.pages()) {
      page_numbers.push_back(number);
    }
    std::sort(page_numbers.begin(), page_numbers.end());
    for (const auto number : page_numbers) {
      writer_.key(std::to_string(number));
      emit_page(document.pages().at(number));
    }
    writer_.end_object();
  }

  void emit_document(const docv1::Document& document) {
    writer_.begin_object();
    writer_.member_string("schema_name", kSchemaName);
    writer_.member_string("version", kSchemaVersion);
    writer_.member_string("name", document.name());
    if (document.has_origin()) emit_origin(document.origin());
    writer_.key("furniture");
    emit_group(document.furniture());
    writer_.key("body");
    emit_group(document.body());

    emit_arena("groups", document.groups(),
               [this](const docv1::GroupItem& item) { emit_group(item); });
    emit_arena("texts", document.texts(),
               [this](const docv1::BaseTextItem& item) { emit_text_variant(item); });
    emit_arena("pictures", document.pictures(),
               [this](const docv1::PictureItem& item) { emit_picture(item); });
    emit_arena("tables", document.tables(),
               [this](const docv1::TableItem& item) { emit_table(item); });
    emit_arena("key_value_items", document.key_value_items(),
               [this](const auto& item) { emit_graph_item(item, "key_value_region"); });
    emit_arena("form_items", document.form_items(),
               [this](const auto& item) { emit_graph_item(item, "form"); });

    // The field arenas are suppressed when empty (unlike the arenas above,
    // which always dump).
    if (!document.field_regions().empty()) {
      emit_arena("field_regions", document.field_regions(), [this](const auto& item) {
        emit_field_container_item(item, "field_region");
      });
    }
    if (!document.field_items().empty()) {
      emit_arena("field_items", document.field_items(), [this](const auto& item) {
        emit_field_container_item(item, "field_item");
      });
    }

    emit_pages(document);
    writer_.end_object();
  }
};

}  // namespace

std::string render_canonical_json(const ai::pipestream::document::v1::Document& document) {
  // The reference model normalizes a loaded document before dumping it.
  // Copying the whole document for that is expensive (page images and
  // picture crops dominate), so the copy happens only when a normalization
  // would actually change something; otherwise the emitter reads the
  // caller's document directly.
  if (!render::needs_clamping(document) && !render::has_misplaced_list_items(document) &&
      !render::has_ordered_list_groups(document)) {
    return CanonicalJsonRenderer().render(document);
  }
  ai::pipestream::document::v1::Document normalized = document;
  render::clamp_document(&normalized);
  // Ordered-list groups relabel to plain list groups first, exactly like the
  // reference load; only then can the migration see them as list parents.
  render::relabel_ordered_list_groups(&normalized);
  render::migrate_misplaced_list_items(&normalized);
  return CanonicalJsonRenderer().render(normalized);
}

}  // namespace grparse
