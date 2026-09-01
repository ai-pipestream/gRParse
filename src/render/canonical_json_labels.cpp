#include "canonical_json_labels.h"

#include <optional>
#include <string_view>

#include "renderer_base.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse::render {

std::string_view content_layer_string(docv1::ContentLayer layer) {
  switch (layer) {
    case docv1::CONTENT_LAYER_FURNITURE: return "furniture";
    case docv1::CONTENT_LAYER_BACKGROUND: return "background";
    case docv1::CONTENT_LAYER_INVISIBLE: return "invisible";
    case docv1::CONTENT_LAYER_NOTES: return "notes";
    default: return "body";  // BODY, unspecified, and unknown tags
  }
}

std::optional<std::string_view> doc_item_label_string(docv1::DocItemLabel label) {
  switch (label) {
    case docv1::DOC_ITEM_LABEL_CAPTION: return "caption";
    case docv1::DOC_ITEM_LABEL_CHART: return "chart";
    case docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED: return "checkbox_selected";
    case docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED: return "checkbox_unselected";
    case docv1::DOC_ITEM_LABEL_CODE: return "code";
    case docv1::DOC_ITEM_LABEL_DOCUMENT_INDEX: return "document_index";
    case docv1::DOC_ITEM_LABEL_EMPTY_VALUE: return "empty_value";
    case docv1::DOC_ITEM_LABEL_FOOTNOTE: return "footnote";
    case docv1::DOC_ITEM_LABEL_FORM: return "form";
    case docv1::DOC_ITEM_LABEL_FORMULA: return "formula";
    case docv1::DOC_ITEM_LABEL_GRADING_SCALE: return "grading_scale";
    case docv1::DOC_ITEM_LABEL_HANDWRITTEN_TEXT: return "handwritten_text";
    case docv1::DOC_ITEM_LABEL_KEY_VALUE_REGION: return "key_value_region";
    case docv1::DOC_ITEM_LABEL_LIST_ITEM: return "list_item";
    case docv1::DOC_ITEM_LABEL_PAGE_FOOTER: return "page_footer";
    case docv1::DOC_ITEM_LABEL_PAGE_HEADER: return "page_header";
    case docv1::DOC_ITEM_LABEL_PARAGRAPH: return "paragraph";
    case docv1::DOC_ITEM_LABEL_PICTURE: return "picture";
    case docv1::DOC_ITEM_LABEL_REFERENCE: return "reference";
    case docv1::DOC_ITEM_LABEL_SECTION_HEADER: return "section_header";
    case docv1::DOC_ITEM_LABEL_TABLE: return "table";
    case docv1::DOC_ITEM_LABEL_TEXT: return "text";
    case docv1::DOC_ITEM_LABEL_TITLE: return "title";
    case docv1::DOC_ITEM_LABEL_FIELD_REGION: return "field_region";
    case docv1::DOC_ITEM_LABEL_FIELD_HEADING: return "field_heading";
    case docv1::DOC_ITEM_LABEL_FIELD_ITEM: return "field_item";
    case docv1::DOC_ITEM_LABEL_FIELD_KEY: return "field_key";
    case docv1::DOC_ITEM_LABEL_FIELD_VALUE: return "field_value";
    case docv1::DOC_ITEM_LABEL_FIELD_HINT: return "field_hint";
    case docv1::DOC_ITEM_LABEL_MARKER: return "marker";
    default: return std::nullopt;
  }
}

std::string_view group_label_string(docv1::GroupLabel label) {
  switch (label) {
    case docv1::GROUP_LABEL_LIST: return "list";
    case docv1::GROUP_LABEL_ORDERED_LIST: return "ordered_list";
    case docv1::GROUP_LABEL_CHAPTER: return "chapter";
    case docv1::GROUP_LABEL_SECTION: return "section";
    case docv1::GROUP_LABEL_SHEET: return "sheet";
    case docv1::GROUP_LABEL_SLIDE: return "slide";
    case docv1::GROUP_LABEL_FORM_AREA: return "form_area";
    case docv1::GROUP_LABEL_KEY_VALUE_AREA: return "key_value_area";
    case docv1::GROUP_LABEL_COMMENT_SECTION: return "comment_section";
    case docv1::GROUP_LABEL_INLINE: return "inline";
    case docv1::GROUP_LABEL_PICTURE_AREA: return "picture_area";
    default: return "unspecified";
  }
}

std::string_view script_string(docv1::Script script) {
  switch (script) {
    case docv1::SCRIPT_SUB: return "sub";
    case docv1::SCRIPT_SUPER: return "super";
    default: return "baseline";  // BASELINE, unspecified, and unknown tags
  }
}

std::string_view coord_origin_string(const docv1::BoundingBox& bbox) {
  // Tag 0, with or without the raw fallback, keeps the model default.
  if (bbox.has_coord_origin() &&
      bbox.coord_origin() == docv1::COORD_ORIGIN_BOTTOMLEFT) {
    return "BOTTOMLEFT";
  }
  return "TOPLEFT";
}

std::string_view orientation_string(docv1::Orientation orientation) {
  switch (orientation) {
    case docv1::ORIENTATION_ROT_90: return "rot_90";
    case docv1::ORIENTATION_ROT_180: return "rot_180";
    case docv1::ORIENTATION_ROT_270: return "rot_270";
    default: return "rot_0";  // ROT_0, unspecified, and unknown tags
  }
}

std::string_view graph_cell_label_string(docv1::GraphCellLabel label) {
  switch (label) {
    case docv1::GRAPH_CELL_LABEL_KEY: return "key";
    case docv1::GRAPH_CELL_LABEL_VALUE: return "value";
    case docv1::GRAPH_CELL_LABEL_CHECKBOX: return "checkbox";
    default: return "unspecified";
  }
}

std::string_view graph_link_label_string(docv1::GraphLinkLabel label) {
  switch (label) {
    case docv1::GRAPH_LINK_LABEL_TO_VALUE: return "to_value";
    case docv1::GRAPH_LINK_LABEL_TO_KEY: return "to_key";
    case docv1::GRAPH_LINK_LABEL_TO_PARENT: return "to_parent";
    case docv1::GRAPH_LINK_LABEL_TO_CHILD: return "to_child";
    default: return "unspecified";
  }
}

// The exported code language: a recognized tag maps directly; anything else
// (unset, unspecified, an unknown tag, or a raw-only fallback) collapses to
// the vocabulary's catch-all "unknown", which is also the model default.
std::string_view exported_code_language(docv1::CodeLanguageLabel tag) {
  return code_language_string(tag).value_or("unknown");
}

}  // namespace grparse::render
