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
#include "canonical_json_writer.h"

namespace grparse {
namespace {

namespace docv1 = ai::pipestream::document::v1;
using render::JsonWriter;
using render::canonical_double;
using render::canonical_integral_decimal;

// The canonical dialect identity. The wire schema_name/version are
// service-internal and deliberately not echoed: the export always declares
// the dialect's own schema name and the schema version this renderer
// implements.
constexpr std::string_view kSchemaName = "DoclingDocument";
constexpr std::string_view kSchemaVersion = "1.10.0";

// ---------------------------------------------------------------------------
// Enum tag -> canonical string tables. Unknown tags fall back to the model
// default the import path would produce, never to zero-value pollution.
// ---------------------------------------------------------------------------

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

std::optional<std::string_view> code_language_string(docv1::CodeLanguageLabel tag) {
  switch (tag) {
    case docv1::CODE_LANGUAGE_LABEL_ADA: return "Ada";
    case docv1::CODE_LANGUAGE_LABEL_AWK: return "Awk";
    case docv1::CODE_LANGUAGE_LABEL_BASH: return "Bash";
    case docv1::CODE_LANGUAGE_LABEL_BC: return "bc";
    case docv1::CODE_LANGUAGE_LABEL_C: return "C";
    case docv1::CODE_LANGUAGE_LABEL_C_SHARP: return "C#";
    case docv1::CODE_LANGUAGE_LABEL_C_PLUS_PLUS: return "C++";
    case docv1::CODE_LANGUAGE_LABEL_CMAKE: return "CMake";
    case docv1::CODE_LANGUAGE_LABEL_COBOL: return "COBOL";
    case docv1::CODE_LANGUAGE_LABEL_CSS: return "CSS";
    case docv1::CODE_LANGUAGE_LABEL_CEYLON: return "Ceylon";
    case docv1::CODE_LANGUAGE_LABEL_CLOJURE: return "Clojure";
    case docv1::CODE_LANGUAGE_LABEL_CRYSTAL: return "Crystal";
    case docv1::CODE_LANGUAGE_LABEL_CUDA: return "Cuda";
    case docv1::CODE_LANGUAGE_LABEL_CYTHON: return "Cython";
    case docv1::CODE_LANGUAGE_LABEL_D: return "D";
    case docv1::CODE_LANGUAGE_LABEL_DART: return "Dart";
    case docv1::CODE_LANGUAGE_LABEL_DC: return "dc";
    case docv1::CODE_LANGUAGE_LABEL_DOCKERFILE: return "Dockerfile";
    case docv1::CODE_LANGUAGE_LABEL_ELIXIR: return "Elixir";
    case docv1::CODE_LANGUAGE_LABEL_ERLANG: return "Erlang";
    case docv1::CODE_LANGUAGE_LABEL_FORTRAN: return "FORTRAN";
    case docv1::CODE_LANGUAGE_LABEL_FORTH: return "Forth";
    case docv1::CODE_LANGUAGE_LABEL_GO: return "Go";
    case docv1::CODE_LANGUAGE_LABEL_HTML: return "HTML";
    case docv1::CODE_LANGUAGE_LABEL_HASKELL: return "Haskell";
    case docv1::CODE_LANGUAGE_LABEL_HAXE: return "Haxe";
    case docv1::CODE_LANGUAGE_LABEL_JAVA: return "Java";
    case docv1::CODE_LANGUAGE_LABEL_JAVASCRIPT: return "JavaScript";
    case docv1::CODE_LANGUAGE_LABEL_JSON: return "JSON";
    case docv1::CODE_LANGUAGE_LABEL_JULIA: return "Julia";
    case docv1::CODE_LANGUAGE_LABEL_KOTLIN: return "Kotlin";
    case docv1::CODE_LANGUAGE_LABEL_LISP: return "Lisp";
    case docv1::CODE_LANGUAGE_LABEL_LUA: return "Lua";
    case docv1::CODE_LANGUAGE_LABEL_MATLAB: return "Matlab";
    case docv1::CODE_LANGUAGE_LABEL_MOONSCRIPT: return "MoonScript";
    case docv1::CODE_LANGUAGE_LABEL_NIM: return "Nim";
    case docv1::CODE_LANGUAGE_LABEL_OCAML: return "OCaml";
    case docv1::CODE_LANGUAGE_LABEL_OBJECTIVEC: return "ObjectiveC";
    case docv1::CODE_LANGUAGE_LABEL_OCTAVE: return "Octave";
    case docv1::CODE_LANGUAGE_LABEL_PHP: return "PHP";
    case docv1::CODE_LANGUAGE_LABEL_PASCAL: return "Pascal";
    case docv1::CODE_LANGUAGE_LABEL_PERL: return "Perl";
    case docv1::CODE_LANGUAGE_LABEL_PROLOG: return "Prolog";
    case docv1::CODE_LANGUAGE_LABEL_PYTHON: return "Python";
    case docv1::CODE_LANGUAGE_LABEL_RACKET: return "Racket";
    case docv1::CODE_LANGUAGE_LABEL_RUBY: return "Ruby";
    case docv1::CODE_LANGUAGE_LABEL_RUST: return "Rust";
    case docv1::CODE_LANGUAGE_LABEL_SML: return "SML";
    case docv1::CODE_LANGUAGE_LABEL_SQL: return "SQL";
    case docv1::CODE_LANGUAGE_LABEL_SCALA: return "Scala";
    case docv1::CODE_LANGUAGE_LABEL_SCHEME: return "Scheme";
    case docv1::CODE_LANGUAGE_LABEL_SWIFT: return "Swift";
    case docv1::CODE_LANGUAGE_LABEL_TYPESCRIPT: return "TypeScript";
    case docv1::CODE_LANGUAGE_LABEL_UNKNOWN: return "unknown";
    case docv1::CODE_LANGUAGE_LABEL_VISUALBASIC: return "VisualBasic";
    case docv1::CODE_LANGUAGE_LABEL_XML: return "XML";
    case docv1::CODE_LANGUAGE_LABEL_YAML: return "YAML";
    case docv1::CODE_LANGUAGE_LABEL_LATEX: return "Latex";
    case docv1::CODE_LANGUAGE_LABEL_TIKZ: return "Tikz";
    case docv1::CODE_LANGUAGE_LABEL_DOCLANG: return "DocLang";
    default: return std::nullopt;
  }
}

// The BCP-47 code is the lowercase suffix of the proto enum name.
std::optional<std::string> human_language_string(docv1::HumanLanguageLabel tag) {
  if (tag == docv1::HUMAN_LANGUAGE_LABEL_UNSPECIFIED ||
      !docv1::HumanLanguageLabel_IsValid(tag)) {
    return std::nullopt;
  }
  constexpr std::string_view prefix = "HUMAN_LANGUAGE_LABEL_";
  std::string name = docv1::HumanLanguageLabel_Name(tag);
  if (!name.starts_with(prefix)) return std::nullopt;
  std::string code = name.substr(prefix.size());
  std::transform(code.begin(), code.end(), code.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return code;
}

// The exported code language: a recognized tag maps directly; anything else
// (unset, unspecified, an unknown tag, or a raw-only fallback) collapses to
// the vocabulary's catch-all "unknown", which is also the model default.
std::string_view exported_code_language(docv1::CodeLanguageLabel tag) {
  return code_language_string(tag).value_or("unknown");
}

// ---------------------------------------------------------------------------
// URI normalization. The model layer parses hyperlink/uri strings through a
// URL type whose serializer normalizes them; the states this service and its
// collectors produce are covered here: scheme lowercasing and, for the
// special schemes, host lowercasing plus an explicit "/" path when the path
// is empty. Strings without a scheme pass through untouched (path
// semantics). Exotic normalizations (percent-encoding, IDNA) are out of
// scope and would surface in the validation oracle if a producer ever hit
// them.
// ---------------------------------------------------------------------------

bool is_special_scheme(std::string_view scheme) {
  return scheme == "http" || scheme == "https" || scheme == "ws" ||
         scheme == "wss" || scheme == "ftp" || scheme == "file";
}

std::string normalized_uri(const std::string& uri) {
  // Scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
  std::size_t colon = std::string::npos;
  if (!uri.empty() && std::isalpha(static_cast<unsigned char>(uri[0]))) {
    for (std::size_t i = 1; i < uri.size(); ++i) {
      const char c = uri[i];
      if (c == ':') {
        colon = i;
        break;
      }
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' &&
          c != '.') {
        break;
      }
    }
  }
  if (colon == std::string::npos) return uri;

  std::string scheme = uri.substr(0, colon);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::string rest = uri.substr(colon + 1);
  if (!is_special_scheme(scheme) || !rest.starts_with("//")) {
    return scheme + ":" + rest;
  }

  const std::size_t authority_start = 2;
  std::size_t authority_end = rest.find_first_of("/?#", authority_start);
  if (authority_end == std::string::npos) authority_end = rest.size();
  std::string authority =
      rest.substr(authority_start, authority_end - authority_start);
  // Lowercase the host: the part after any userinfo and before any port.
  const std::size_t host_start =
      authority.rfind('@') == std::string::npos ? 0 : authority.rfind('@') + 1;
  std::size_t host_end = authority.find(':', host_start);
  if (host_end == std::string::npos) host_end = authority.size();
  std::transform(authority.begin() + static_cast<std::ptrdiff_t>(host_start),
                 authority.begin() + static_cast<std::ptrdiff_t>(host_end),
                 authority.begin() + static_cast<std::ptrdiff_t>(host_start),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::string tail = rest.substr(authority_end);
  if (tail.empty() || tail[0] == '?' || tail[0] == '#') tail.insert(0, "/");
  return scheme + "://" + authority + tail;
}

// ---------------------------------------------------------------------------
// Load normalization. The reference model applies two mutations while
// loading a document, before any dump: provenance bounding boxes are clamped
// to their page, and list items whose parent is not a list group are moved
// into a synthesized list group (re-appended at the end of the text arena,
// with every reference renumbered). Both are reproduced here on a private
// copy so the rendered output matches the reference dump exactly.
// ---------------------------------------------------------------------------

double clamp_coordinate(double value, double hi) {
  return std::min(std::max(value, 0.0), std::max(hi, 0.0));
}

void clamp_bbox(docv1::BoundingBox* bbox, const docv1::Size& page) {
  bbox->set_l(clamp_coordinate(bbox->l(), page.width()));
  bbox->set_r(clamp_coordinate(bbox->r(), page.width()));
  bbox->set_t(clamp_coordinate(bbox->t(), page.height()));
  bbox->set_b(clamp_coordinate(bbox->b(), page.height()));
}

void clamp_prov_list(docv1::Document* doc,
                     google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* provs) {
  for (auto& prov : *provs) {
    const auto page = doc->pages().find(prov.page_no());
    if (page != doc->pages().end()) clamp_bbox(prov.mutable_bbox(), page->second.size());
  }
}

template <typename Message>
void clamp_graph_item(docv1::Document* doc, Message* item) {
  clamp_prov_list(doc, item->mutable_prov());
  for (auto& cell : *item->mutable_graph()->mutable_cells()) {
    if (!cell.has_prov()) continue;
    const auto page = doc->pages().find(cell.prov().page_no());
    if (page != doc->pages().end()) {
      clamp_bbox(cell.mutable_prov()->mutable_bbox(), page->second.size());
    }
  }
}

void clamp_document(docv1::Document* doc) {
  for (auto& text : *doc->mutable_texts()) {
    switch (text.item_case()) {
      case docv1::BaseTextItem::kTitle:
        clamp_prov_list(doc, text.mutable_title()->mutable_base()->mutable_prov());
        break;
      case docv1::BaseTextItem::kSectionHeader:
        clamp_prov_list(doc, text.mutable_section_header()->mutable_base()->mutable_prov());
        break;
      case docv1::BaseTextItem::kFieldHeading:
        clamp_prov_list(doc, text.mutable_field_heading()->mutable_base()->mutable_prov());
        break;
      case docv1::BaseTextItem::kFieldValue:
        clamp_prov_list(doc, text.mutable_field_value()->mutable_base()->mutable_prov());
        break;
      case docv1::BaseTextItem::kListItem:
        clamp_prov_list(doc, text.mutable_list_item()->mutable_base()->mutable_prov());
        break;
      case docv1::BaseTextItem::kCode:
        clamp_prov_list(doc, text.mutable_code()->mutable_prov());
        break;
      case docv1::BaseTextItem::kFormula:
        clamp_prov_list(doc, text.mutable_formula()->mutable_base()->mutable_prov());
        break;
      case docv1::BaseTextItem::kText:
        clamp_prov_list(doc, text.mutable_text()->mutable_base()->mutable_prov());
        break;
      default:
        break;
    }
  }
  for (auto& picture : *doc->mutable_pictures()) {
    clamp_prov_list(doc, picture.mutable_prov());
  }
  for (auto& table : *doc->mutable_tables()) {
    clamp_prov_list(doc, table.mutable_prov());
    // Cell boxes clamp only when every table provenance names one page and
    // that page is known.
    std::optional<std::int32_t> page_no;
    bool single_page = !table.prov().empty();
    for (const auto& prov : table.prov()) {
      if (page_no && *page_no != prov.page_no()) single_page = false;
      page_no = prov.page_no();
    }
    if (!single_page || !page_no) continue;
    const auto page = doc->pages().find(*page_no);
    if (page == doc->pages().end()) continue;
    for (auto& cell : *table.mutable_data()->mutable_table_cells()) {
      if (cell.has_bbox()) clamp_bbox(cell.mutable_bbox(), page->second.size());
    }
  }
  for (auto& item : *doc->mutable_key_value_items()) clamp_graph_item(doc, &item);
  for (auto& item : *doc->mutable_form_items()) clamp_graph_item(doc, &item);
  for (auto& item : *doc->mutable_field_regions()) clamp_prov_list(doc, item.mutable_prov());
  for (auto& item : *doc->mutable_field_items()) clamp_prov_list(doc, item.mutable_prov());
}

// A parsed "#/<arena>/<index>" reference; arena stays empty for roots and
// anything else that is not a three-part reference.
struct RefParts {
  std::string arena;
  int index = -1;
};

RefParts parse_ref_parts(std::string_view ref) {
  RefParts out;
  if (!ref.starts_with("#/")) return out;
  const std::size_t slash = ref.find('/', 2);
  if (slash == std::string_view::npos) return out;
  const std::string_view index_text = ref.substr(slash + 1);
  if (index_text.empty()) return out;
  int index = 0;
  for (const char c : index_text) {
    if (c < '0' || c > '9') return out;
    index = index * 10 + (c - '0');
  }
  out.arena = std::string(ref.substr(2, slash - 2));
  out.index = index;
  return out;
}

// Mutable views of the node fields the normalization touches. Null members
// mean the node type has no such field (or the reference did not resolve).
struct NodeFields {
  bool resolved = false;
  std::string* self_ref = nullptr;
  docv1::RefItem* parent = nullptr;  // null when unset or absent
  google::protobuf::RepeatedPtrField<docv1::RefItem>* children = nullptr;
  google::protobuf::RepeatedPtrField<docv1::FineRef>* comments = nullptr;
  google::protobuf::RepeatedPtrField<docv1::RefItem>* captions = nullptr;
  google::protobuf::RepeatedPtrField<docv1::RefItem>* references = nullptr;
  google::protobuf::RepeatedPtrField<docv1::RefItem>* footnotes = nullptr;
  docv1::TableData* table_data = nullptr;
};

template <typename Message>
NodeFields doc_item_fields(Message* item) {
  NodeFields out;
  out.resolved = true;
  out.self_ref = item->mutable_self_ref();
  if (item->has_parent()) out.parent = item->mutable_parent();
  out.children = item->mutable_children();
  out.comments = item->mutable_comments();
  return out;
}

template <typename Message>
NodeFields floating_item_fields(Message* item) {
  NodeFields out = doc_item_fields(item);
  out.captions = item->mutable_captions();
  out.references = item->mutable_references();
  out.footnotes = item->mutable_footnotes();
  return out;
}

NodeFields group_fields(docv1::GroupItem* group) {
  NodeFields out;
  out.resolved = true;
  out.self_ref = group->mutable_self_ref();
  if (group->has_parent()) out.parent = group->mutable_parent();
  out.children = group->mutable_children();
  return out;
}

const docv1::TextItemBase* const_text_base(const docv1::BaseTextItem& text) {
  switch (text.item_case()) {
    case docv1::BaseTextItem::kTitle: return &text.title().base();
    case docv1::BaseTextItem::kSectionHeader: return &text.section_header().base();
    case docv1::BaseTextItem::kFieldHeading: return &text.field_heading().base();
    case docv1::BaseTextItem::kFieldValue: return &text.field_value().base();
    case docv1::BaseTextItem::kListItem: return &text.list_item().base();
    case docv1::BaseTextItem::kFormula: return &text.formula().base();
    case docv1::BaseTextItem::kText: return &text.text().base();
    default: return nullptr;  // kCode inlines its base; unset has none
  }
}

docv1::TextItemBase* mutable_text_base(docv1::BaseTextItem* text) {
  switch (text->item_case()) {
    case docv1::BaseTextItem::kTitle: return text->mutable_title()->mutable_base();
    case docv1::BaseTextItem::kSectionHeader:
      return text->mutable_section_header()->mutable_base();
    case docv1::BaseTextItem::kFieldHeading:
      return text->mutable_field_heading()->mutable_base();
    case docv1::BaseTextItem::kFieldValue:
      return text->mutable_field_value()->mutable_base();
    case docv1::BaseTextItem::kListItem: return text->mutable_list_item()->mutable_base();
    case docv1::BaseTextItem::kFormula: return text->mutable_formula()->mutable_base();
    case docv1::BaseTextItem::kText: return text->mutable_text()->mutable_base();
    default: return nullptr;
  }
}

NodeFields node_fields(docv1::Document* doc, std::string_view ref) {
  if (ref == "#/body") return group_fields(doc->mutable_body());
  if (ref == "#/furniture") return group_fields(doc->mutable_furniture());
  const RefParts parts = parse_ref_parts(ref);
  const auto in_range = [&parts](int size) {
    return parts.index >= 0 && parts.index < size;
  };
  if (parts.arena == "groups" && in_range(doc->groups_size())) {
    return group_fields(doc->mutable_groups(parts.index));
  }
  if (parts.arena == "texts" && in_range(doc->texts_size())) {
    auto* text = doc->mutable_texts(parts.index);
    if (text->item_case() == docv1::BaseTextItem::kCode) {
      return floating_item_fields(text->mutable_code());
    }
    if (auto* base = mutable_text_base(text)) return doc_item_fields(base);
    return {};
  }
  if (parts.arena == "pictures" && in_range(doc->pictures_size())) {
    return floating_item_fields(doc->mutable_pictures(parts.index));
  }
  if (parts.arena == "tables" && in_range(doc->tables_size())) {
    NodeFields out = floating_item_fields(doc->mutable_tables(parts.index));
    out.table_data = doc->mutable_tables(parts.index)->mutable_data();
    return out;
  }
  if (parts.arena == "key_value_items" && in_range(doc->key_value_items_size())) {
    return floating_item_fields(doc->mutable_key_value_items(parts.index));
  }
  if (parts.arena == "form_items" && in_range(doc->form_items_size())) {
    return floating_item_fields(doc->mutable_form_items(parts.index));
  }
  if (parts.arena == "field_regions" && in_range(doc->field_regions_size())) {
    return doc_item_fields(doc->mutable_field_regions(parts.index));
  }
  if (parts.arena == "field_items" && in_range(doc->field_items_size())) {
    return doc_item_fields(doc->mutable_field_items(parts.index));
  }
  return {};
}

// Deleted indices per arena, in the model's renumbering formula: a
// three-part reference shifts down by the number of deleted indices at or
// below it.
using DeleteLookup = std::map<std::string, std::vector<int>, std::less<>>;

std::string renumbered_ref(const std::string& ref, const DeleteLookup& lookup) {
  const RefParts parts = parse_ref_parts(ref);
  if (parts.arena.empty()) return ref;
  const auto deleted = lookup.find(parts.arena);
  if (deleted == lookup.end()) return ref;
  int delta = 0;
  for (const int index : deleted->second) {
    if (parts.index >= index) --delta;
  }
  return "#/" + parts.arena + "/" + std::to_string(parts.index + delta);
}

// True when the text entry reconstructs as a list item: the dedicated arm,
// or the generic arm carrying the list-item label.
bool is_list_item_entry(const docv1::BaseTextItem& text) {
  if (text.item_case() == docv1::BaseTextItem::kListItem) return true;
  return text.item_case() == docv1::BaseTextItem::kText &&
         text.text().base().label() == docv1::DOC_ITEM_LABEL_LIST_ITEM;
}

bool is_list_group_ref(const docv1::Document* doc, std::string_view ref) {
  if (ref == "#/body") return doc->body().label() == docv1::GROUP_LABEL_LIST;
  if (ref == "#/furniture") return doc->furniture().label() == docv1::GROUP_LABEL_LIST;
  const RefParts parts = parse_ref_parts(ref);
  return parts.arena == "groups" && parts.index < doc->groups_size() &&
         doc->groups(parts.index).label() == docv1::GROUP_LABEL_LIST;
}

// ---------------------------------------------------------------------------
// Normalization detection. The render path copies the document only when one
// of the two load-time normalizations would actually change it; these
// predicates mirror clamp_document and the run collector CONSERVATIVELY. A
// false positive only costs the defensive copy; a false negative would skip
// a required normalization, so every approximation errs toward true. All
// reads are const, which keeps the zero-copy path safe for concurrent
// renders of one shared document (the emitter itself never mutates).
// ---------------------------------------------------------------------------

bool bbox_out_of_bounds(const docv1::BoundingBox& bbox, const docv1::Size& page) {
  return bbox.l() != clamp_coordinate(bbox.l(), page.width()) ||
         bbox.r() != clamp_coordinate(bbox.r(), page.width()) ||
         bbox.t() != clamp_coordinate(bbox.t(), page.height()) ||
         bbox.b() != clamp_coordinate(bbox.b(), page.height());
}

bool prov_list_needs_clamping(
    const docv1::Document& doc,
    const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>& provs) {
  for (const auto& prov : provs) {
    const auto page = doc.pages().find(prov.page_no());
    if (page != doc.pages().end() &&
        bbox_out_of_bounds(prov.bbox(), page->second.size())) {
      return true;
    }
  }
  return false;
}

bool needs_clamping(const docv1::Document& doc) {
  for (const auto& text : doc.texts()) {
    const google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* provs = nullptr;
    if (text.item_case() == docv1::BaseTextItem::kCode) {
      provs = &text.code().prov();
    } else if (const auto* base = const_text_base(text)) {
      provs = &base->prov();
    }
    if (provs != nullptr && prov_list_needs_clamping(doc, *provs)) return true;
  }
  for (const auto& picture : doc.pictures()) {
    if (prov_list_needs_clamping(doc, picture.prov())) return true;
  }
  for (const auto& table : doc.tables()) {
    if (prov_list_needs_clamping(doc, table.prov())) return true;
    std::optional<std::int32_t> page_no;
    bool single_page = !table.prov().empty();
    for (const auto& prov : table.prov()) {
      if (page_no && *page_no != prov.page_no()) single_page = false;
      page_no = prov.page_no();
    }
    if (!single_page || !page_no) continue;
    const auto page = doc.pages().find(*page_no);
    if (page == doc.pages().end()) continue;
    for (const auto& cell : table.data().table_cells()) {
      if (cell.has_bbox() && bbox_out_of_bounds(cell.bbox(), page->second.size())) {
        return true;
      }
    }
  }
  const auto graph_needs = [&doc](const auto& item) {
    if (prov_list_needs_clamping(doc, item.prov())) return true;
    for (const auto& cell : item.graph().cells()) {
      if (!cell.has_prov()) continue;
      const auto page = doc.pages().find(cell.prov().page_no());
      if (page != doc.pages().end() &&
          bbox_out_of_bounds(cell.prov().bbox(), page->second.size())) {
        return true;
      }
    }
    return false;
  };
  for (const auto& item : doc.key_value_items()) {
    if (graph_needs(item)) return true;
  }
  for (const auto& item : doc.form_items()) {
    if (graph_needs(item)) return true;
  }
  for (const auto& item : doc.field_regions()) {
    if (prov_list_needs_clamping(doc, item.prov())) return true;
  }
  for (const auto& item : doc.field_items()) {
    if (prov_list_needs_clamping(doc, item.prov())) return true;
  }
  return false;
}

// A linear over-approximation of the run collector: any list item whose
// parent is not a list group forces the migration pass. Items unreachable
// from the body would not actually migrate; treating them as if they would
// only costs the copy.
bool has_misplaced_list_items(const docv1::Document& doc) {
  for (const auto& text : doc.texts()) {
    if (!is_list_item_entry(text)) continue;
    const docv1::TextItemBase& base =
        text.item_case() == docv1::BaseTextItem::kListItem
            ? text.list_item().base()
            : text.text().base();
    if (base.has_parent() && !is_list_group_ref(&doc, base.parent().ref())) {
      return true;
    }
  }
  return false;
}

// Applies the delete renumbering to one node and recurses over its children
// (body tree only, exactly like the model: unreachable nodes keep stale
// references). Exact-match children/caption/reference/footnote entries to
// the deleted run refs are removed; comments and rich cell references are
// renumbered without removal.
void renumber_subtree(docv1::Document* doc, const std::string& ref,
                      const std::vector<std::string>& run_refs,
                      const DeleteLookup& lookup,
                      std::set<std::string>* visited) {
  if (!visited->insert(ref).second) return;
  NodeFields fields = node_fields(doc, ref);
  if (!fields.resolved) return;

  const auto is_run_ref = [&run_refs](const std::string& candidate) {
    return std::find(run_refs.begin(), run_refs.end(), candidate) != run_refs.end();
  };
  const auto filter_and_renumber =
      [&](google::protobuf::RepeatedPtrField<docv1::RefItem>* refs) {
        if (refs == nullptr) return;
        for (int i = refs->size() - 1; i >= 0; --i) {
          if (is_run_ref(refs->Get(i).ref())) {
            refs->DeleteSubrange(i, 1);
          } else {
            refs->Mutable(i)->set_ref(renumbered_ref(refs->Get(i).ref(), lookup));
          }
        }
      };

  if (fields.comments != nullptr) {
    for (auto& fine : *fields.comments) {
      fine.set_ref(renumbered_ref(fine.ref(), lookup));
    }
  }
  filter_and_renumber(fields.captions);
  filter_and_renumber(fields.references);
  filter_and_renumber(fields.footnotes);
  if (fields.table_data != nullptr) {
    for (auto& cell : *fields.table_data->mutable_table_cells()) {
      if (cell.has_ref()) {
        cell.mutable_ref()->set_ref(renumbered_ref(cell.ref().ref(), lookup));
      }
    }
  }
  if (fields.parent != nullptr) {
    fields.parent->set_ref(renumbered_ref(fields.parent->ref(), lookup));
  }
  *fields.self_ref = renumbered_ref(*fields.self_ref, lookup);
  filter_and_renumber(fields.children);

  const std::vector<std::string> child_refs = [&fields] {
    std::vector<std::string> out;
    for (const auto& child : *fields.children) out.push_back(child.ref());
    return out;
  }();
  for (const auto& child : child_refs) {
    renumber_subtree(doc, child, run_refs, lookup, visited);
  }
}

// Collects the subtree rooted at `ref` (the node itself plus every
// descendant reachable through children) into per-arena deletion sets.
void collect_subtree(docv1::Document* doc, const std::string& ref,
                     std::map<std::string, std::set<int>>* deleted,
                     std::set<std::string>* visited) {
  if (!visited->insert(ref).second) return;
  const RefParts parts = parse_ref_parts(ref);
  if (!parts.arena.empty()) (*deleted)[parts.arena].insert(parts.index);
  NodeFields fields = node_fields(doc, ref);
  if (!fields.resolved || fields.children == nullptr) return;
  const std::vector<std::string> child_refs = [&fields] {
    std::vector<std::string> out;
    for (const auto& child : *fields.children) out.push_back(child.ref());
    return out;
  }();
  for (const auto& child : child_refs) collect_subtree(doc, child, deleted, visited);
}

void delete_arena_entries(docv1::Document* doc, const std::string& arena,
                          const std::set<int>& indices) {
  const auto erase_descending = [&indices](auto* repeated) {
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
      if (*it >= 0 && *it < repeated->size()) repeated->DeleteSubrange(*it, 1);
    }
  };
  if (arena == "groups") erase_descending(doc->mutable_groups());
  else if (arena == "texts") erase_descending(doc->mutable_texts());
  else if (arena == "pictures") erase_descending(doc->mutable_pictures());
  else if (arena == "tables") erase_descending(doc->mutable_tables());
  else if (arena == "key_value_items") erase_descending(doc->mutable_key_value_items());
  else if (arena == "form_items") erase_descending(doc->mutable_form_items());
  else if (arena == "field_regions") erase_descending(doc->mutable_field_regions());
  else if (arena == "field_items") erase_descending(doc->mutable_field_items());
}

// Depth-first collection of misplaced list-item runs, mirroring the model's
// iteration: every node in the body tree in pre-order, runs continuing only
// while the previous visited node was a list item parented directly at the
// body (or parentless).
struct RunCollector {
  docv1::Document* doc = nullptr;
  std::vector<std::vector<int>> runs;
  bool prev_is_list_item = false;
  bool prev_parent_body_or_none = false;
  std::set<std::string> visited;

  void visit(const std::string& ref) {
    if (!visited.insert(ref).second) return;
    bool is_list_item = false;
    bool parent_body_or_none = false;
    const RefParts parts = parse_ref_parts(ref);
    if (parts.arena == "texts" && parts.index < doc->texts_size() &&
        is_list_item_entry(doc->texts(parts.index))) {
      is_list_item = true;
      const NodeFields fields = node_fields(doc, ref);
      const bool has_parent = fields.parent != nullptr;
      parent_body_or_none = !has_parent || fields.parent->ref() == "#/body";
      const bool misplaced =
          !has_parent || !is_list_group_ref(doc, fields.parent->ref());
      if (misplaced && has_parent) {  // a parentless item cannot be re-homed
        if (prev_is_list_item && prev_parent_body_or_none && !runs.empty()) {
          runs.back().push_back(parts.index);
        } else {
          runs.push_back({parts.index});
        }
      }
    }
    prev_is_list_item = is_list_item;
    prev_parent_body_or_none = parent_body_or_none;

    NodeFields fields = node_fields(doc, ref);
    if (!fields.resolved || fields.children == nullptr) return;
    const std::vector<std::string> child_refs = [&fields] {
      std::vector<std::string> out;
      for (const auto& child : *fields.children) out.push_back(child.ref());
      return out;
    }();
    for (const auto& child : child_refs) visit(child);
  }
};

// The list-item payload carried over into the synthesized group: everything
// the model's re-add copies (first provenance only), with metadata, sources,
// comments, and children left behind.
struct SalvagedListItem {
  docv1::TextItemBase base;
  bool enumerated = false;
  std::string marker = "-";
};

SalvagedListItem salvage_list_item(const docv1::BaseTextItem& text) {
  SalvagedListItem out;
  if (text.item_case() == docv1::BaseTextItem::kListItem) {
    out.base = text.list_item().base();
    out.enumerated = text.list_item().enumerated();
    if (text.list_item().has_marker()) out.marker = text.list_item().marker();
  } else {
    out.base = text.text().base();
  }
  return out;
}

void migrate_misplaced_list_items(docv1::Document* doc) {
  RunCollector collector;
  collector.doc = doc;
  collector.visit("#/body");
  if (collector.runs.empty()) return;

  // Later runs are re-homed first, exactly like the model.
  for (auto run = collector.runs.rbegin(); run != collector.runs.rend(); ++run) {
    const std::string first_ref = "#/texts/" + std::to_string((*run)[0]);
    const NodeFields first_fields = node_fields(doc, first_ref);
    if (!first_fields.resolved || first_fields.parent == nullptr) continue;
    const std::string parent_ref = first_fields.parent->ref();
    NodeFields parent_fields = node_fields(doc, parent_ref);
    if (!parent_fields.resolved || parent_fields.children == nullptr) continue;

    // Salvage the run's payloads before anything moves.
    std::vector<SalvagedListItem> salvaged;
    std::vector<std::string> run_refs;
    for (const int index : *run) {
      salvaged.push_back(salvage_list_item(doc->texts(index)));
      run_refs.push_back("#/texts/" + std::to_string(index));
    }

    // Insert the new list group before the run's first item.
    const std::string group_ref = "#/groups/" + std::to_string(doc->groups_size());
    auto* group = doc->add_groups();
    group->set_self_ref(group_ref);
    group->mutable_parent()->set_ref(parent_ref);
    group->set_content_layer(docv1::CONTENT_LAYER_BODY);
    group->set_name("group");
    group->set_label(docv1::GROUP_LABEL_LIST);
    parent_fields = node_fields(doc, parent_ref);  // pointers may have moved
    int position = parent_fields.children->size();
    for (int i = 0; i < parent_fields.children->size(); ++i) {
      if (parent_fields.children->Get(i).ref() == first_ref) {
        position = i;
        break;
      }
    }
    parent_fields.children->Add()->set_ref(group_ref);
    for (int i = parent_fields.children->size() - 1; i > position; --i) {
      parent_fields.children->SwapElements(i, i - 1);
    }

    // Delete the run items and their subtrees.
    std::map<std::string, std::set<int>> deleted;
    {
      std::set<std::string> visited;
      for (const auto& ref : run_refs) collect_subtree(doc, ref, &deleted, &visited);
    }
    for (const auto& [arena, indices] : deleted) {
      for (const int index : indices) {
        const std::string ref = "#/" + arena + "/" + std::to_string(index);
        const NodeFields fields = node_fields(doc, ref);
        if (!fields.resolved || fields.parent == nullptr) continue;
        NodeFields owner = node_fields(doc, fields.parent->ref());
        if (!owner.resolved || owner.children == nullptr) continue;
        for (int i = 0; i < owner.children->size(); ++i) {
          if (owner.children->Get(i).ref() == ref) {
            owner.children->DeleteSubrange(i, 1);
            break;
          }
        }
      }
    }
    DeleteLookup lookup;
    for (const auto& [arena, indices] : deleted) {
      lookup[arena].assign(indices.begin(), indices.end());
      delete_arena_entries(doc, arena, indices);
    }
    {
      std::set<std::string> visited;
      renumber_subtree(doc, "#/body", run_refs, lookup, &visited);
    }
    // Earlier (still pending) runs shift with the same renumbering.
    for (auto pending = std::next(run); pending != collector.runs.rend(); ++pending) {
      for (int& index : *pending) {
        const RefParts parts = parse_ref_parts(
            renumbered_ref("#/texts/" + std::to_string(index), lookup));
        index = parts.index;
      }
    }

    // Re-append the salvaged items under the group, in run order.
    const std::string final_group_ref =
        "#/groups/" + std::to_string(doc->groups_size() - 1);
    auto* final_group = doc->mutable_groups(doc->groups_size() - 1);
    for (const auto& item : salvaged) {
      const std::string new_ref = "#/texts/" + std::to_string(doc->texts_size());
      auto* entry = doc->add_texts()->mutable_list_item();
      auto* base = entry->mutable_base();
      base->set_self_ref(new_ref);
      base->mutable_parent()->set_ref(final_group_ref);
      base->set_content_layer(item.base.content_layer());
      base->set_label(docv1::DOC_ITEM_LABEL_LIST_ITEM);
      if (!item.base.prov().empty()) *base->add_prov() = item.base.prov(0);
      base->set_orig(!item.base.orig().empty() ? item.base.orig() : item.base.text());
      base->set_text(item.base.text());
      if (item.base.has_formatting()) *base->mutable_formatting() = item.base.formatting();
      if (item.base.has_hyperlink()) base->set_hyperlink(item.base.hyperlink());
      entry->set_enumerated(item.enumerated);
      entry->set_marker(item.marker);
      final_group->add_children()->set_ref(new_ref);
    }
  }
}

// ---------------------------------------------------------------------------
// The renderer.
// ---------------------------------------------------------------------------

class CanonicalJsonRenderer {
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
  using ValueMap = google::protobuf::Map<std::string, google::protobuf::Value>;

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

  // True when the key already satisfies the namespace__field_name rule.
  static bool conforming_custom_name(std::string_view key) {
    const std::size_t pos = key.find("__");
    return pos != std::string_view::npos && pos > 0 && pos + 2 < key.size();
  }

  // Emits the map's entries as trailing members of the current object,
  // mirroring the bridge: non-conforming names move under the "pipestream"
  // namespace (every character outside [A-Za-z0-9_] folded to "_", collision
  // suffixes _2, _3, ...) and entries whose payload is null are dropped.
  // The wire map is unordered, so entries are emitted in byte order of the
  // final keys for a deterministic export.
  void emit_custom_fields(const ValueMap& fields) {
    if (fields.empty()) return;
    std::vector<std::pair<std::string, const google::protobuf::Value*>> entries;
    entries.reserve(fields.size());
    for (const auto& [key, value] : fields) entries.emplace_back(key, &value);
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::string> taken;
    taken.reserve(entries.size());
    for (const auto& entry : entries) taken.push_back(entry.first);
    for (auto& [key, value] : entries) {
      if (conforming_custom_name(key)) continue;
      std::string base = "pipestream__";
      for (const char c : key) {
        const auto byte = static_cast<unsigned char>(c);
        // One replacement per character: UTF-8 continuation bytes fold into
        // their lead byte's underscore.
        if ((byte & 0xc0) == 0x80) continue;
        const bool word = (byte < 0x80 && std::isalnum(byte)) || c == '_';
        base.push_back(word ? c : '_');
      }
      std::string candidate = base;
      int suffix = 2;
      while (std::find(taken.begin(), taken.end(), candidate) != taken.end()) {
        candidate = base + "_" + std::to_string(suffix++);
      }
      *std::find(taken.begin(), taken.end(), key) = candidate;
      key = std::move(candidate);
    }

    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [key, value] : entries) {
      // A null payload at custom-field level vanishes under the dump's
      // exclude-none rule; nulls nested inside values survive.
      if (value->kind_case() == google::protobuf::Value::kNullValue ||
          value->kind_case() == google::protobuf::Value::KIND_NOT_SET) {
        continue;
      }
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
    for (const auto& value : meta.values()) writer_.value_string(value);
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

  void emit_picture_meta(const docv1::PictureMeta& meta) {
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
    emit_custom_fields(meta.custom_fields());
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
    // dimensions; the wire grid is a redundant projection and is ignored.
    // Spanned cells repeat at every position they cover, and grid entries
    // render as plain cells (no ref, matching the computed field's type).
    const int num_rows = data.num_rows();
    const int num_cols = data.num_cols();
    std::vector<const docv1::TableCell*> grid(
        static_cast<std::size_t>(std::max(num_rows, 0)) *
            static_cast<std::size_t>(std::max(num_cols, 0)),
        nullptr);
    for (const auto& cell : data.table_cells()) {
      const int row_begin = std::clamp(cell.start_row_offset_idx(), 0, num_rows);
      const int row_end = std::clamp(cell.end_row_offset_idx(), 0, num_rows);
      const int col_begin = std::clamp(cell.start_col_offset_idx(), 0, num_cols);
      const int col_end = std::clamp(cell.end_col_offset_idx(), 0, num_cols);
      for (int row = row_begin; row < row_end; ++row) {
        for (int column = col_begin; column < col_end; ++column) {
          grid[static_cast<std::size_t>(row) * num_cols + column] = &cell;
        }
      }
    }
    writer_.key("grid");
    writer_.begin_array();
    for (int row = 0; row < num_rows; ++row) {
      writer_.begin_array();
      for (int column = 0; column < num_cols; ++column) {
        const auto* cell = grid[static_cast<std::size_t>(row) * num_cols + column];
        if (cell != nullptr) {
          emit_table_cell(*cell, false);
        } else {
          emit_grid_filler_cell(row, column);
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
    if (picture.has_meta()) emit_picture_meta(picture.meta());
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

    writer_.key("groups");
    writer_.begin_array();
    for (const auto& group : document.groups()) emit_group(group);
    writer_.end_array();

    writer_.key("texts");
    writer_.begin_array();
    for (const auto& text : document.texts()) emit_text_variant(text);
    writer_.end_array();

    writer_.key("pictures");
    writer_.begin_array();
    for (const auto& picture : document.pictures()) emit_picture(picture);
    writer_.end_array();

    writer_.key("tables");
    writer_.begin_array();
    for (const auto& table : document.tables()) emit_table(table);
    writer_.end_array();

    writer_.key("key_value_items");
    writer_.begin_array();
    for (const auto& item : document.key_value_items()) {
      emit_graph_item(item, "key_value_region");
    }
    writer_.end_array();

    writer_.key("form_items");
    writer_.begin_array();
    for (const auto& item : document.form_items()) emit_graph_item(item, "form");
    writer_.end_array();

    // The field arenas are suppressed when empty (unlike the arenas above,
    // which always dump).
    if (!document.field_regions().empty()) {
      writer_.key("field_regions");
      writer_.begin_array();
      for (const auto& item : document.field_regions()) {
        emit_field_container_item(item, "field_region");
      }
      writer_.end_array();
    }
    if (!document.field_items().empty()) {
      writer_.key("field_items");
      writer_.begin_array();
      for (const auto& item : document.field_items()) {
        emit_field_container_item(item, "field_item");
      }
      writer_.end_array();
    }

    // Integer-keyed pages dump under their decimal keys in ascending
    // numeric order.
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
  if (!needs_clamping(document) && !has_misplaced_list_items(document)) {
    return CanonicalJsonRenderer().render(document);
  }
  ai::pipestream::document::v1::Document normalized = document;
  clamp_document(&normalized);
  migrate_misplaced_list_items(&normalized);
  return CanonicalJsonRenderer().render(normalized);
}

}  // namespace grparse
