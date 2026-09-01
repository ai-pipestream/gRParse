#include "grparse/office_fold/form_fold.h"

#include <algorithm>
#include <cctype>

namespace grparse::office_fold {

namespace {

// The field's own kind, in the office core's vocabulary when it names one.
std::string field_kind(const officev1::FormField& field) {
  if (!field.field_type().empty()) return field.field_type();
  std::string kind = officev1::FormFieldKind_Name(field.kind());
  const std::string prefix = "FORM_FIELD_KIND_";
  if (kind.starts_with(prefix)) kind = kind.substr(prefix.size());
  std::ranges::transform(kind, kind.begin(),
                         [](unsigned char c) { return std::tolower(c); });
  return kind;
}

// One fieldmark parameter as the string map the schema keeps them in. A
// list keeps one entry per key rather than a joined string, so no separator
// has to be guessed back out on the way in.
void set_parameter(const officev1::FormFieldParameter& parameter,
                   google::protobuf::Map<std::string, std::string>* out) {
  switch (parameter.value_case()) {
    case officev1::FormFieldParameter::kBoolValue:
      (*out)[parameter.name()] = parameter.bool_value() ? "true" : "false";
      return;
    case officev1::FormFieldParameter::kIntValue:
      (*out)[parameter.name()] = std::to_string(parameter.int_value());
      return;
    case officev1::FormFieldParameter::kDoubleValue:
      (*out)[parameter.name()] = std::to_string(parameter.double_value());
      return;
    case officev1::FormFieldParameter::kStringValue:
      (*out)[parameter.name()] = parameter.string_value();
      return;
    case officev1::FormFieldParameter::VALUE_NOT_SET:
      if (parameter.string_list().empty()) {
        (*out)[parameter.name()] = std::string();
        return;
      }
      for (int entry = 0; entry < parameter.string_list_size(); entry++) {
        (*out)[parameter.name() + "[" + std::to_string(entry) + "]"] =
            parameter.string_list(entry);
      }
      return;
  }
}

}  // namespace

void FormFold::ensure_form_arena() {
  if (!field_region_ref_.empty()) return;
  docv1::Document& document = arena_.document();
  docv1::FieldRegionItem* region = document.add_field_regions();
  region->set_self_ref("#/field_regions/0");
  region->mutable_parent()->set_ref("#/body");
  region->set_label(docv1::DOC_ITEM_LABEL_FIELD_REGION);
  region->set_content_layer(docv1::CONTENT_LAYER_BODY);
  arena_.stamp_collector_source(region->mutable_source());
  field_region_ref_ = region->self_ref();
  arena_.link_child("#/body", field_region_ref_);

  docv1::FormItem* form = document.add_form_items();
  form->set_self_ref("#/form_items/0");
  form->mutable_parent()->set_ref("#/body");
  form->set_label(docv1::DOC_ITEM_LABEL_FORM);
  form->set_content_layer(docv1::CONTENT_LAYER_BODY);
  arena_.stamp_collector_source(form->mutable_source());
  arena_.link_child("#/body", form->self_ref());
}

docv1::FieldItem* FormFold::add_field_item(std::string* field_ref) {
  docv1::Document& document = arena_.document();
  *field_ref =
      "#/field_items/" + std::to_string(document.field_items_size());
  docv1::FieldItem* item = document.add_field_items();
  item->set_self_ref(*field_ref);
  item->mutable_parent()->set_ref(field_region_ref_);
  item->set_label(docv1::DOC_ITEM_LABEL_FIELD_ITEM);
  item->set_content_layer(docv1::CONTENT_LAYER_BODY);
  arena_.stamp_collector_source(item->mutable_source());
  arena_.link_child(field_region_ref_, *field_ref);
  return item;
}

void FormFold::link_graph(const std::string& heading_ref,
                          const std::string& label,
                          const std::string& value_ref,
                          const std::string& text, bool checkbox) {
  docv1::GraphData* graph =
      arena_.document().mutable_form_items(0)->mutable_graph();
  int key_cell = -1;
  if (!heading_ref.empty()) {
    docv1::GraphCell* cell = graph->add_cells();
    cell->set_label(docv1::GRAPH_CELL_LABEL_KEY);
    key_cell = graph_cell_id_++;
    cell->set_cell_id(key_cell);
    cell->set_text(label);
    cell->set_orig(label);
    cell->mutable_item_ref()->set_ref(heading_ref);
  }
  docv1::GraphCell* value_cell = graph->add_cells();
  value_cell->set_label(checkbox ? docv1::GRAPH_CELL_LABEL_CHECKBOX
                                 : docv1::GRAPH_CELL_LABEL_VALUE);
  const int value_cell_id = graph_cell_id_++;
  value_cell->set_cell_id(value_cell_id);
  value_cell->set_text(text);
  value_cell->set_orig(text);
  value_cell->mutable_item_ref()->set_ref(value_ref);
  if (key_cell < 0) return;
  docv1::GraphLink* link = graph->add_links();
  link->set_label(docv1::GRAPH_LINK_LABEL_TO_VALUE);
  link->set_source_cell_id(key_cell);
  link->set_target_cell_id(value_cell_id);
}

void FormFold::set_identity(const officev1::FormField& field,
                            docv1::FieldItem* item) {
  if (!field.name().empty()) item->set_field_name(field.name());
  for (const std::string& entry : field.list_entries()) {
    item->add_options(entry);
  }
  if (field.selected_index() >= 0) {
    item->set_selected_index(field.selected_index());
  }
  auto* parameters = item->mutable_parameters();
  for (const officev1::FormFieldParameter& parameter : field.parameters()) {
    set_parameter(parameter, parameters);
  }
}

void FormFold::add_field_prov(const officev1::FormField& field,
                              docv1::FieldItem* item,
                              const TextHandle& value) {
  if (field.control() && field.width_twips() > 0 && field.has_anchor()) {
    arena_.add_prov(item->mutable_prov(), field.page_index(), false,
                    static_cast<double>(field.anchor().x()),
                    static_cast<double>(field.anchor().y()),
                    static_cast<double>(field.anchor().x() + field.width_twips()),
                    static_cast<double>(field.anchor().y() + field.height_twips()),
                    0, 0);
  } else {
    arena_.add_caret_prov(item->mutable_prov(), field.page_index(),
                          field.anchor(), field.anchor(), 0, 0);
  }
  for (const docv1::ProvenanceItem& prov : item->prov()) {
    *value.base->add_prov() = prov;
  }
}

void FormFold::on_form_field(const officev1::FormField& field) {
  ensure_form_arena();
  // One field item per office form field, holding its heading and its
  // value: the schema's own form subtree rather than a text item with a bag
  // of attributes hanging off it.
  const int field_index = arena_.document().field_items_size();
  std::string field_ref;
  docv1::FieldItem* item = add_field_item(&field_ref);

  const bool checkbox = field.kind() == officev1::FORM_FIELD_KIND_CHECKBOX;
  std::string heading_ref;
  if (!field.label().empty()) {
    TextHandle heading = arena_.add_text(TextKind::kFieldHeading,
                                         docv1::DOC_ITEM_LABEL_FIELD_KEY,
                                         docv1::CONTENT_LAYER_BODY, field_ref);
    heading.base->set_text(field.label());
    heading.base->set_orig(field.label());
    heading_ref = heading.ref;
  }

  docv1::DocItemLabel value_label = docv1::DOC_ITEM_LABEL_FIELD_VALUE;
  if (checkbox) {
    value_label = field.checked() ? docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED
                                  : docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED;
  }
  TextHandle value = arena_.add_text(TextKind::kFieldValue, value_label,
                                     docv1::CONTENT_LAYER_BODY, field_ref);
  // A checkbox renders no text of its own; its state is its label.
  std::string text = field.text();
  if (text.empty() && !checkbox) text = field.label();
  value.base->set_text(text);
  value.base->set_orig(text);
  value.item->mutable_field_value()->set_kind(field_kind(field));

  link_graph(heading_ref, field.label(), value.ref, text, checkbox);
  set_identity(field, item);
  if (field.char_start() >= 0) {
    // The span resolves against the body index once the whole body has
    // streamed past, like every other anchor.
    anchors_.add_field_span(field_index, field.char_start(),
                            field.char_end());
  }
  add_field_prov(field, item, value);
}

}  // namespace grparse::office_fold
