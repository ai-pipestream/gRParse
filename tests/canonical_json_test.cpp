#include <cmath>
#include <cstdlib>
#include <cstring>
#include <print>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../src/render/canonical_json_writer.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"

namespace docv1 = ai::pipestream::document::v1;

using grparse::render::canonical_double;
using grparse::render::canonical_integral_decimal;
using grparse::render::escape_json_ascii;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_eq(const std::string& actual, const std::string& expected,
                const std::string& message) {
  if (actual != expected) {
    throw std::runtime_error(message + "\nexpected: " + expected +
                             "\nactual:   " + actual);
  }
}

void require_contains(const std::string& haystack, const std::string& needle,
                      const std::string& message) {
  if (!haystack.contains(needle)) {
    throw std::runtime_error(message + " (missing: " + needle + ")\nrendered:\n" +
                             haystack);
  }
}

void require_absent(const std::string& haystack, const std::string& needle,
                    const std::string& message) {
  if (haystack.contains(needle)) {
    throw std::runtime_error(message + " (unexpected: " + needle + ")\nrendered:\n" +
                             haystack);
  }
}

// -- number formatting ------------------------------------------------------

void verify_double_formatting_matches_reference_repr() {
  const struct {
    double value;
    const char* expected;
  } cases[] = {
      {0.0, "0.0"},
      {-0.0, "-0.0"},
      {1.0, "1.0"},
      {-1.0, "-1.0"},
      {0.1, "0.1"},
      {0.5, "0.5"},
      {2.5, "2.5"},
      {2.675, "2.675"},
      {1.5, "1.5"},
      {0.0001, "0.0001"},
      {1e-4, "0.0001"},
      {1e-5, "1e-05"},
      {1.25e-5, "1.25e-05"},
      {5e-324, "5e-324"},
      {2.2250738585072014e-308, "2.2250738585072014e-308"},
      {1e15, "1000000000000000.0"},
      {999999999999999.9, "999999999999999.9"},
      {1e16, "1e+16"},
      {1.5e16, "1.5e+16"},
      {9007199254740993.0, "9007199254740992.0"},
      {1e100, "1e+100"},
      {1.7976931348623157e308, "1.7976931348623157e+308"},
      {-1234.5, "-1234.5"},
      {123456789012345.67, "123456789012345.67"},
      {100.0, "100.0"},
      {200.5, "200.5"},
      {2.0, "2.0"},
      {3.14159, "3.14159"},
      {1.0 / 3.0, "0.3333333333333333"},
      {2.0 / 3.0, "0.6666666666666666"},
  };
  for (const auto& [value, expected] : cases) {
    require_eq(canonical_double(value), expected,
               "double formatting must match the reference repr");
  }
}

void verify_integral_decimal_is_exact() {
  const struct {
    double value;
    const char* expected;
  } cases[] = {
      {0.0, "0"},
      {-0.0, "0"},
      {3.0, "3"},
      {-42.0, "-42"},
      {9007199254740992.0, "9007199254740992"},
      {1e20, "100000000000000000000"},
      // 1e23 is not exactly representable; the dump prints the exact value
      // of the nearest double, not a rounded power of ten.
      {1e23, "99999999999999991611392"},
      {18446744073709551616.0, "18446744073709551616"},
  };
  for (const auto& [value, expected] : cases) {
    require_eq(canonical_integral_decimal(value), expected,
               "integral decimal must be the exact double value");
  }
}

// -- string escaping --------------------------------------------------------

void verify_ascii_escaping_matches_reference_dump() {
  require_eq(escape_json_ascii("plain"), "plain", "plain text passes through");
  require_eq(escape_json_ascii("a\"b\\c"), "a\\\"b\\\\c", "specials escape");
  require_eq(escape_json_ascii("a/b"), "a/b", "slash never escapes");
  require_eq(escape_json_ascii("\b\t\n\f\r"), "\\b\\t\\n\\f\\r",
             "control shorthands");
  require_eq(escape_json_ascii(std::string(1, '\x01')), "\\u0001",
             "other controls escape as u-sequences");
  require_eq(escape_json_ascii("\x7f"), "\\u007f", "DEL escapes");
  require_eq(escape_json_ascii("\xc3\xa9"), "\\u00e9", "two-byte sequences");
  require_eq(escape_json_ascii("\xe2\x82\xac"), "\\u20ac", "three-byte sequences");
  require_eq(escape_json_ascii("\xf0\x9f\x98\x80"), "\\ud83d\\ude00",
             "astral characters become surrogate pairs");
  require_eq(escape_json_ascii("\xff"), "\\ufffd", "stray bytes degrade");
}

// -- document fixtures ------------------------------------------------------

docv1::Document base_document(const std::string& name) {
  docv1::Document document;
  document.set_schema_name("docling_document_v2");  // wire-internal, replaced
  document.set_version("0.0.0");                    // wire-internal, replaced
  document.set_name(name);
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_body()->set_name("_root_");
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  document.mutable_furniture()->set_name("_root_");
  return document;
}

docv1::TextItemBase* text_base(docv1::Document* document,
                               docv1::BaseTextItem::ItemCase variant,
                               docv1::DocItemLabel label, const std::string& text) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* item = document->add_texts();
  docv1::TextItemBase* base = nullptr;
  switch (variant) {
    case docv1::BaseTextItem::kTitle: base = item->mutable_title()->mutable_base(); break;
    case docv1::BaseTextItem::kSectionHeader:
      base = item->mutable_section_header()->mutable_base();
      break;
    case docv1::BaseTextItem::kListItem:
      base = item->mutable_list_item()->mutable_base();
      break;
    case docv1::BaseTextItem::kFormula: base = item->mutable_formula()->mutable_base(); break;
    case docv1::BaseTextItem::kFieldHeading:
      base = item->mutable_field_heading()->mutable_base();
      break;
    case docv1::BaseTextItem::kFieldValue:
      base = item->mutable_field_value()->mutable_base();
      break;
    default: base = item->mutable_text()->mutable_base(); break;
  }
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_label(label);
  base->set_orig(text);
  base->set_text(text);
  document->mutable_body()->add_children()->set_ref(ref);
  return base;
}

// -- whole-document layout --------------------------------------------------

void verify_empty_document_layout_and_identity() {
  const docv1::Document document = base_document("empty");
  const std::string expected =
      "{\n"
      "  \"schema_name\": \"DoclingDocument\",\n"
      "  \"version\": \"1.10.0\",\n"
      "  \"name\": \"empty\",\n"
      "  \"furniture\": {\n"
      "    \"self_ref\": \"#/furniture\",\n"
      "    \"children\": [],\n"
      "    \"content_layer\": \"furniture\",\n"
      "    \"name\": \"_root_\",\n"
      "    \"label\": \"unspecified\"\n"
      "  },\n"
      "  \"body\": {\n"
      "    \"self_ref\": \"#/body\",\n"
      "    \"children\": [],\n"
      "    \"content_layer\": \"body\",\n"
      "    \"name\": \"_root_\",\n"
      "    \"label\": \"unspecified\"\n"
      "  },\n"
      "  \"groups\": [],\n"
      "  \"texts\": [],\n"
      "  \"pictures\": [],\n"
      "  \"tables\": [],\n"
      "  \"key_value_items\": [],\n"
      "  \"form_items\": [],\n"
      "  \"pages\": {}\n"
      "}";
  require_eq(grparse::render_canonical_json(document), expected,
             "empty document must match the canonical layout byte for byte");
}

void verify_identity_header_is_constant() {
  docv1::Document document = base_document("ident");
  document.clear_schema_name();
  document.clear_version();
  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered, "\"schema_name\": \"DoclingDocument\"",
                   "schema name is the dialect constant");
  require_contains(rendered, "\"version\": \"1.10.0\"",
                   "version is the dialect constant");
  require_absent(rendered, "docling_document_v2", "wire schema name never leaks");
}

// -- text variants ----------------------------------------------------------

void verify_text_variants_flatten_with_subclass_members() {
  docv1::Document document = base_document("texts");
  text_base(&document, docv1::BaseTextItem::kTitle, docv1::DOC_ITEM_LABEL_TITLE,
            "Title");
  auto* header = text_base(&document, docv1::BaseTextItem::kSectionHeader,
                           docv1::DOC_ITEM_LABEL_SECTION_HEADER, "Header");
  document.mutable_texts(1)->mutable_section_header()->set_level(3);
  header->mutable_formatting()->set_bold(true);
  text_base(&document, docv1::BaseTextItem::kListItem,
            docv1::DOC_ITEM_LABEL_LIST_ITEM, "Item");
  auto* list = document.mutable_texts(2)->mutable_list_item();
  list->set_enumerated(true);
  list->set_marker("1.");
  text_base(&document, docv1::BaseTextItem::kFormula,
            docv1::DOC_ITEM_LABEL_FORMULA, "x^2");
  text_base(&document, docv1::BaseTextItem::kFieldHeading,
            docv1::DOC_ITEM_LABEL_FIELD_HEADING, "FH");
  text_base(&document, docv1::BaseTextItem::kFieldValue,
            docv1::DOC_ITEM_LABEL_FIELD_VALUE, "FV");
  document.mutable_texts(5)->mutable_field_value()->set_kind("fillable");
  auto* plain = text_base(&document, docv1::BaseTextItem::kText,
                          docv1::DOC_ITEM_LABEL_PARAGRAPH, "Para");
  plain->set_hyperlink("HTTP://Example.COM");

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered, "\"label\": \"title\"", "title arm label");
  require_contains(rendered, "\"label\": \"section_header\"", "header label");
  require_contains(rendered, "\"level\": 3", "explicit header level");
  require_contains(rendered,
                   "\"enumerated\": true,\n      \"marker\": \"1.\"",
                   "list item members in order");
  require_contains(rendered, "\"label\": \"formula\"", "formula label");
  require_contains(rendered, "\"label\": \"field_heading\"", "field heading label");
  require_contains(rendered, "\"level\": 1", "field heading default level");
  require_contains(rendered, "\"kind\": \"fillable\"", "field value kind");
  require_contains(rendered, "\"label\": \"paragraph\"", "generic text label");
  require_contains(rendered, "\"hyperlink\": \"http://example.com/\"",
                   "hyperlink normalizes scheme, host, and empty path");
  require_contains(
      rendered,
      "\"formatting\": {\n        \"bold\": true,\n        \"italic\": false,\n"
      "        \"underline\": false,\n        \"strikethrough\": false,\n"
      "        \"script\": \"baseline\"\n      }",
      "formatting dumps all five members");
}

void verify_generic_arm_dispatches_on_label() {
  docv1::Document document = base_document("dispatch");
  // A generic text arm whose label names a subclass reconstructs that
  // subclass with default subclass members.
  text_base(&document, docv1::BaseTextItem::kText,
            docv1::DOC_ITEM_LABEL_SECTION_HEADER, "H");
  text_base(&document, docv1::BaseTextItem::kText,
            docv1::DOC_ITEM_LABEL_LIST_ITEM, "L");
  auto* code = text_base(&document, docv1::BaseTextItem::kText,
                         docv1::DOC_ITEM_LABEL_CODE, "x = 1");
  code->mutable_meta()->mutable_summary()->set_text("code summary");
  text_base(&document, docv1::BaseTextItem::kText,
            docv1::DOC_ITEM_LABEL_UNSPECIFIED, "plain");

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered, "\"label\": \"section_header\"",
                   "label dispatch to section header");
  require_contains(rendered, "\"level\": 1", "dispatched header gets default level");
  require_contains(rendered,
                   "\"enumerated\": false,\n      \"marker\": \"-\"",
                   "dispatched list item gets default members");
  require_contains(rendered, "\"label\": \"code\"", "label dispatch to code");
  require_contains(rendered, "\"code_language\": \"unknown\"",
                   "dispatched code gets the default language");
  require_contains(rendered,
                   "\"captions\": [],\n      \"references\": [],\n"
                   "      \"footnotes\": [],\n      \"code_language\": \"unknown\"",
                   "dispatched code gains the floating members with defaults");
  require_contains(rendered, "\"summary\": {\n          \"text\": \"code summary\"",
                   "base meta carries over into the code shape");
  require_contains(rendered, "\"label\": \"text\"",
                   "unset label defaults to text");
}

void verify_code_item_inlined_base_flattens() {
  docv1::Document document = base_document("code");
  auto* item = document.add_texts();
  auto* code = item->mutable_code();
  code->set_self_ref("#/texts/0");
  code->mutable_parent()->set_ref("#/body");
  code->set_content_layer(docv1::CONTENT_LAYER_BODY);
  code->set_label(docv1::DOC_ITEM_LABEL_CODE);
  code->set_orig("print(1)");
  code->set_text("print(1)");
  code->set_code_language(docv1::CODE_LANGUAGE_LABEL_C_PLUS_PLUS);
  code->mutable_meta()->mutable_description()->set_text("a snippet");
  document.mutable_body()->add_children()->set_ref("#/texts/0");

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered, "\"code_language\": \"C++\"",
                   "recognized languages use the canonical spelling");
  require_contains(rendered, "\"description\": {\n          \"text\": \"a snippet\"",
                   "code meta is the floating shape");
  require_contains(rendered,
                   "\"captions\": [],\n      \"references\": [],\n"
                   "      \"footnotes\": [],\n      \"code_language\": \"C++\"",
                   "floating members precede the language");

  // Raw-only fallback: tag 0 with a raw string collapses to the catch-all.
  code->set_code_language(docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED);
  code->set_code_language_raw("Zig");
  require_contains(grparse::render_canonical_json(document),
                   "\"code_language\": \"unknown\"",
                   "raw-only language falls back to unknown");
}

// -- provenance, charspans, coordinate origins ------------------------------

void verify_prov_charspan_and_coord_origins() {
  docv1::Document document = base_document("prov");
  auto* base = text_base(&document, docv1::BaseTextItem::kText,
                         docv1::DOC_ITEM_LABEL_TEXT, "T");
  auto* prov = base->add_prov();
  prov->set_page_no(2);
  prov->mutable_bbox()->set_l(1.5);
  prov->mutable_bbox()->set_t(2);
  prov->mutable_bbox()->set_r(3);
  prov->mutable_bbox()->set_b(4);
  prov->mutable_bbox()->set_coord_origin(docv1::COORD_ORIGIN_BOTTOMLEFT);
  prov->mutable_charspan()->set_start(5);
  prov->mutable_charspan()->set_end(9);
  auto* second = base->add_prov();
  second->set_page_no(3);
  second->mutable_bbox()->set_l(0);
  second->mutable_bbox()->set_t(0);
  second->mutable_bbox()->set_r(1);
  second->mutable_bbox()->set_b(1);

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered,
                   "\"charspan\": [\n            5,\n            9\n          ]",
                   "charspans dump as two-element arrays");
  require_contains(rendered, "\"coord_origin\": \"BOTTOMLEFT\"",
                   "explicit origin maps");
  require_contains(rendered, "\"coord_origin\": \"TOPLEFT\"",
                   "unset origin keeps the model default");
  require_contains(rendered, "\"l\": 1.5", "coordinates keep repr formatting");
  require_contains(rendered, "\"t\": 2.0", "integral coordinates keep .0");
}

// -- tables -----------------------------------------------------------------

void verify_table_grid_spans_headers_and_rich_cells() {
  docv1::Document document = base_document("table");
  auto* table = document.add_tables();
  table->set_self_ref("#/tables/0");
  table->mutable_parent()->set_ref("#/body");
  table->set_content_layer(docv1::CONTENT_LAYER_BODY);
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  document.mutable_body()->add_children()->set_ref("#/tables/0");
  auto* data = table->mutable_data();
  data->set_num_rows(2);
  data->set_num_cols(2);
  data->set_orientation(docv1::ORIENTATION_ROT_90);
  auto* span = data->add_table_cells();
  span->set_text("span");
  span->set_row_span(2);
  span->set_col_span(1);
  span->set_start_row_offset_idx(0);
  span->set_end_row_offset_idx(2);
  span->set_start_col_offset_idx(0);
  span->set_end_col_offset_idx(1);
  span->set_column_header(true);
  auto* rich = data->add_table_cells();
  rich->set_text("rich");
  rich->set_row_span(1);
  rich->set_col_span(1);
  rich->set_start_row_offset_idx(0);
  rich->set_end_row_offset_idx(1);
  rich->set_start_col_offset_idx(1);
  rich->set_end_col_offset_idx(2);
  rich->set_row_header(true);
  rich->mutable_ref()->set_ref("#/texts/0");
  text_base(&document, docv1::BaseTextItem::kText, docv1::DOC_ITEM_LABEL_TEXT,
            "cell body");

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered, "\"orientation\": \"rot_90\"", "orientation maps");
  require_contains(rendered, "\"column_header\": true", "headers survive");
  require_contains(rendered, "\"row_header\": true", "row headers survive");
  require_contains(rendered,
                   "\"ref\": {\n              \"$ref\": \"#/texts/0\"\n            }",
                   "rich cells keep their ref in table_cells");
  // The spanned cell repeats at both covered grid rows; the uncovered
  // bottom-right position is a default filler cell; grid entries carry no
  // ref (the computed grid narrows rich cells to plain ones).
  const std::size_t spans = [&rendered] {
    std::size_t count = 0;
    for (std::size_t at = rendered.find("\"text\": \"span\"");
         at != std::string::npos; at = rendered.find("\"text\": \"span\"", at + 1)) {
      ++count;
    }
    return count;
  }();
  require(spans == 3, "spanned cell appears once flat and twice in the grid");
  require_contains(rendered, "\"start_row_offset_idx\": 1", "filler cell offsets");
  const std::size_t ref_count = [&rendered] {
    std::size_t count = 0;
    for (std::size_t at = rendered.find("\"ref\": {");
         at != std::string::npos; at = rendered.find("\"ref\": {", at + 1)) {
      ++count;
    }
    return count;
  }();
  require(ref_count == 1, "the grid copy of a rich cell drops the ref");
  require_contains(rendered, "\"annotations\": []",
                   "tables always dump the empty annotation list");
}

// -- pictures ---------------------------------------------------------------

void verify_picture_meta_and_annotations() {
  docv1::Document document = base_document("pics");
  auto* picture = document.add_pictures();
  picture->set_self_ref("#/pictures/0");
  picture->mutable_parent()->set_ref("#/body");
  picture->set_content_layer(docv1::CONTENT_LAYER_BODY);
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  document.mutable_body()->add_children()->set_ref("#/pictures/0");
  auto* meta = picture->mutable_meta();
  meta->mutable_description()->set_text("a chart");
  meta->mutable_description()->set_created_by("m");
  auto* prediction = meta->mutable_classification()->add_predictions();
  prediction->set_class_name("bar_chart");
  prediction->set_confidence(0.75);
  meta->mutable_molecule()->set_smi("C1=CC=CC=C1");
  meta->mutable_tabular_chart()->set_title("T");
  meta->mutable_tabular_chart()->mutable_chart_data()->set_num_rows(0);
  meta->mutable_code()->set_text("x");
  meta->mutable_code()->set_language(docv1::CODE_LANGUAGE_LABEL_PYTHON);
  auto* image = picture->mutable_image();
  image->set_mimetype("image/png");
  image->set_dpi(72);
  image->mutable_size()->set_width(10);
  image->mutable_size()->set_height(20);
  image->set_uri("data:image/png;base64,iVBORw0KGgo=");
  // A wire annotation is a projection of meta; the canonical dump never
  // echoes it.
  picture->add_annotations()->mutable_description()->set_text("legacy");

  // A second picture with no meta at all.
  auto* bare = document.add_pictures();
  bare->set_self_ref("#/pictures/1");
  bare->mutable_parent()->set_ref("#/body");
  bare->set_label(docv1::DOC_ITEM_LABEL_CHART);
  document.mutable_body()->add_children()->set_ref("#/pictures/1");

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered, "\"created_by\": \"m\",\n          \"text\": \"a chart\"",
                   "description members keep prediction order");
  require_contains(rendered,
                   "\"confidence\": 0.75,\n              \"class_name\": \"bar_chart\"",
                   "classification predictions keep prediction order");
  require_contains(rendered, "\"smi\": \"C1=CC=CC=C1\"", "molecule meta");
  require_contains(rendered, "\"title\": \"T\"", "tabular chart title");
  require_contains(rendered, "\"language\": \"Python\"", "code meta language");
  require_contains(rendered, "\"uri\": \"data:image/png;base64,iVBORw0KGgo=\"",
                   "data URIs pass through unchanged");
  require_absent(rendered, "legacy", "wire annotations are never echoed");
  require_contains(rendered, "\"label\": \"chart\"", "chart labels are accepted");
  const std::size_t annotation_lists = [&rendered] {
    std::size_t count = 0;
    for (std::size_t at = rendered.find("\"annotations\": []");
         at != std::string::npos;
         at = rendered.find("\"annotations\": []", at + 1)) {
      ++count;
    }
    return count;
  }();
  require(annotation_lists == 2,
          "every picture dumps the empty annotation list");
}

// -- key-value graphs -------------------------------------------------------

void verify_key_value_graphs() {
  docv1::Document document = base_document("kv");
  auto* item = document.add_key_value_items();
  item->set_self_ref("#/key_value_items/0");
  item->mutable_parent()->set_ref("#/body");
  item->set_label(docv1::DOC_ITEM_LABEL_KEY_VALUE_REGION);
  document.mutable_body()->add_children()->set_ref("#/key_value_items/0");
  auto* key_cell = item->mutable_graph()->add_cells();
  key_cell->set_label(docv1::GRAPH_CELL_LABEL_KEY);
  key_cell->set_cell_id(0);
  key_cell->set_text("k");
  key_cell->set_orig("k");
  auto* value_cell = item->mutable_graph()->add_cells();
  value_cell->set_label(docv1::GRAPH_CELL_LABEL_VALUE);
  value_cell->set_cell_id(1);
  value_cell->set_text("v");
  value_cell->set_orig("v");
  value_cell->mutable_item_ref()->set_ref("#/texts/0");
  auto* link = item->mutable_graph()->add_links();
  link->set_label(docv1::GRAPH_LINK_LABEL_TO_VALUE);
  link->set_source_cell_id(0);
  link->set_target_cell_id(1);
  text_base(&document, docv1::BaseTextItem::kText, docv1::DOC_ITEM_LABEL_TEXT, "v");

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered, "\"label\": \"key\"", "graph cell labels map");
  require_contains(rendered, "\"label\": \"to_value\"", "graph link labels map");
  require_contains(rendered,
                   "\"item_ref\": {\n              \"$ref\": \"#/texts/0\"\n            }",
                   "cell item refs dump as references");
  require_contains(rendered,
                   "\"source_cell_id\": 0,\n            \"target_cell_id\": 1",
                   "link endpoints in declaration order");
}

// -- sources ----------------------------------------------------------------

void verify_track_sources_and_collector_dropping() {
  docv1::Document document = base_document("sources");
  auto* timed = text_base(&document, docv1::BaseTextItem::kText,
                          docv1::DOC_ITEM_LABEL_TEXT, "cue");
  auto* track = timed->add_source()->mutable_track();
  track->set_start_time(1.0);
  track->set_end_time(2.5);
  track->set_voice("Speaker 1");
  auto* collector = timed->add_source()->mutable_collector();
  collector->set_collector("grparse");
  collector->set_model("rapidocr");

  auto* attributed = text_base(&document, docv1::BaseTextItem::kText,
                               docv1::DOC_ITEM_LABEL_TEXT, "ocr line");
  attributed->add_source()->mutable_collector()->set_collector("grparse");

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(
      rendered,
      "\"source\": [\n        {\n          \"kind\": \"track\",\n"
      "          \"start_time\": 1.0,\n          \"end_time\": 2.5,\n"
      "          \"voice\": \"Speaker 1\"\n        }\n      ],",
      "track sources gain the kind discriminator; collector entries drop");
  // The second item's list held only collector entries; the emptied list is
  // suppressed. `source` precedes `orig`/`text` in the layout, so nothing
  // after the second item's text may mention it.
  const std::size_t second_item = rendered.find("ocr line");
  require(second_item != std::string::npos &&
              rendered.find("\"source\"", second_item) == std::string::npos,
          "a source list left empty after dropping is suppressed");
  require_absent(rendered, "grparse", "collector attribution never leaks");
}

// -- meta suppression and custom fields -------------------------------------

void verify_meta_suppression_and_defaults() {
  docv1::Document document = base_document("meta");
  auto* base = text_base(&document, docv1::BaseTextItem::kText,
                         docv1::DOC_ITEM_LABEL_TEXT, "T");
  auto* meta = base->mutable_meta();
  // language with an unrepresentable code drops entirely; entities with no
  // mentions drop entirely; keywords/topics with no values drop entirely.
  meta->mutable_language()->set_code(docv1::HUMAN_LANGUAGE_LABEL_UNSPECIFIED);
  meta->mutable_language()->set_code_raw("xx");
  meta->mutable_entities();
  meta->mutable_keywords();
  meta->mutable_topics();

  auto* with_language = text_base(&document, docv1::BaseTextItem::kText,
                                  docv1::DOC_ITEM_LABEL_TEXT, "U");
  with_language->mutable_meta()->mutable_language()->set_code(
      docv1::HUMAN_LANGUAGE_LABEL_EN);
  with_language->mutable_meta()->mutable_keywords()->add_values("kw");

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered, "\"meta\": {},", "emptied meta dumps as {}");
  require_absent(rendered, "\"entities\"", "empty entities suppressed");
  require_contains(rendered, "\"code\": \"en\"", "language codes map");
  require_contains(rendered,
                   "\"keywords\": {\n          \"values\": [\n            \"kw\"",
                   "keyword values dump");
  require_absent(rendered, "\"topics\"", "empty topics suppressed");
}

void verify_custom_field_rekeying_and_values() {
  docv1::Document document = base_document("custom");
  auto* base = text_base(&document, docv1::BaseTextItem::kText,
                         docv1::DOC_ITEM_LABEL_TEXT, "T");
  auto& fields = *base->mutable_meta()->mutable_custom_fields();
  fields["collector_warnings:pdf"].mutable_list_value()->add_values()->set_string_value(
      "w1");
  fields["epub.version"].set_string_value("3.0");
  // Two keys that sanitize to the same name: the collision gets a suffix.
  fields["a.b"].set_number_value(1.0);
  fields["a:b"].set_number_value(2.5);
  // Already conforming keys stay; null payloads drop; integral numbers
  // narrow to integers.
  fields["acme__kept"].set_number_value(3.0);
  fields["acme__gone"].set_null_value(google::protobuf::NULL_VALUE);
  auto* nested =
      fields["acme__nested"].mutable_struct_value();
  (*nested->mutable_fields())["inner"].set_null_value(google::protobuf::NULL_VALUE);

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered, "\"pipestream__collector_warnings_pdf\": [",
                   "non-conforming names move under the pipestream namespace");
  require_contains(rendered, "\"pipestream__epub_version\": \"3.0\"",
                   "dots fold to underscores");
  require_contains(rendered, "\"pipestream__a_b\": 1,",
                   "first collision keeps the base name and narrows to int");
  require_contains(rendered, "\"pipestream__a_b_2\": 2.5",
                   "second collision takes the _2 suffix");
  require_contains(rendered, "\"acme__kept\": 3", "conforming keys stay");
  require_absent(rendered, "acme__gone", "null custom fields drop");
  require_contains(rendered,
                   "\"acme__nested\": {\n          \"inner\": null\n        }",
                   "nested nulls survive");
}

// -- field arenas, empty-list suppression -----------------------------------

void verify_field_arenas_suppressed_when_empty() {
  docv1::Document document = base_document("fields");
  const std::string rendered = grparse::render_canonical_json(document);
  require_absent(rendered, "field_regions", "empty field_regions suppressed");
  require_absent(rendered, "field_items", "empty field_items suppressed");

  auto* region = document.add_field_regions();
  region->set_self_ref("#/field_regions/0");
  region->mutable_parent()->set_ref("#/body");
  region->set_label(docv1::DOC_ITEM_LABEL_FIELD_REGION);
  auto* item = document.add_field_items();
  item->set_self_ref("#/field_items/0");
  item->mutable_parent()->set_ref("#/field_regions/0");
  item->set_label(docv1::DOC_ITEM_LABEL_FIELD_ITEM);
  document.mutable_body()->add_children()->set_ref("#/field_regions/0");

  const std::string populated = grparse::render_canonical_json(document);
  require_contains(populated, "\"field_regions\": [", "populated arena dumps");
  require_contains(populated, "\"label\": \"field_region\"", "region label");
  require_contains(populated, "\"label\": \"field_item\"", "item label");
}

// -- pages ------------------------------------------------------------------

void verify_pages_dump_in_numeric_order() {
  docv1::Document document = base_document("pages");
  for (const int number : {3, 1, 10, 2}) {
    auto& page = (*document.mutable_pages())[number];
    page.set_page_no(number);
    page.mutable_size()->set_width(612);
    page.mutable_size()->set_height(792);
  }
  (*document.mutable_pages())[1].mutable_image()->set_mimetype("image/png");
  (*document.mutable_pages())[1].mutable_image()->set_dpi(144);
  (*document.mutable_pages())[1].mutable_image()->mutable_size()->set_width(1224);
  (*document.mutable_pages())[1].mutable_image()->mutable_size()->set_height(1584);
  (*document.mutable_pages())[1].mutable_image()->set_uri("data:image/png;base64,AA==");

  const std::string rendered = grparse::render_canonical_json(document);
  const std::size_t p1 = rendered.find("\"1\": {");
  const std::size_t p2 = rendered.find("\"2\": {");
  const std::size_t p3 = rendered.find("\"3\": {");
  const std::size_t p10 = rendered.find("\"10\": {");
  require(p1 != std::string::npos && p1 < p2 && p2 < p3 && p3 < p10,
          "pages keyed by decimal strings in numeric order");
  require_contains(rendered, "\"width\": 612.0", "page sizes keep repr floats");
  require_contains(rendered, "\"dpi\": 144", "page images dump");
}

// -- origin and raw label states --------------------------------------------

void verify_origin_and_raw_label_states() {
  docv1::Document document = base_document("origin");
  auto* origin = document.mutable_origin();
  origin->set_mimetype("application/pdf");
  origin->set_binary_hash(18446744073709551613ull);
  origin->set_filename("f.pdf");
  origin->set_uri("s3://bucket/key");

  auto* picture = document.add_pictures();
  picture->set_self_ref("#/pictures/0");
  picture->mutable_parent()->set_ref("#/body");
  // Tag 0 with a raw label: the strict class cannot carry the raw string,
  // so the class default applies.
  picture->set_label(docv1::DOC_ITEM_LABEL_UNSPECIFIED);
  picture->set_label_raw("hologram");
  document.mutable_body()->add_children()->set_ref("#/pictures/0");

  // Picture code meta: a raw-only language collapses to the catch-all, an
  // unrecognized tag with no raw omits the optional member entirely.
  auto* raw_only = document.add_pictures();
  raw_only->set_self_ref("#/pictures/1");
  raw_only->mutable_parent()->set_ref("#/body");
  raw_only->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  raw_only->mutable_meta()->mutable_code()->set_text("a");
  raw_only->mutable_meta()->mutable_code()->set_language_raw("Zig");
  document.mutable_body()->add_children()->set_ref("#/pictures/1");
  auto* unknown_tag = document.add_pictures();
  unknown_tag->set_self_ref("#/pictures/2");
  unknown_tag->mutable_parent()->set_ref("#/body");
  unknown_tag->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  unknown_tag->mutable_meta()->mutable_code()->set_text("b");
  unknown_tag->mutable_meta()->mutable_code()->set_language(
      static_cast<docv1::CodeLanguageLabel>(999));
  document.mutable_body()->add_children()->set_ref("#/pictures/2");

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered, "\"language\": \"unknown\"",
                   "raw-only code meta language collapses to the catch-all");
  require_contains(rendered, "\"text\": \"b\"\n        }",
                   "an unrecognized tag with no raw omits the language");
  require_contains(rendered, "\"binary_hash\": 18446744073709551613",
                   "binary hash keeps full unsigned range");
  require_contains(rendered, "\"uri\": \"s3://bucket/key\"",
                   "non-special schemes pass through");
  require_contains(rendered, "\"label\": \"picture\"",
                   "raw-only labels fall back to the class default");
  require_absent(rendered, "hologram", "raw label strings never leak");
}

// -- load normalizations ----------------------------------------------------

void verify_bboxes_clamp_to_their_page() {
  docv1::Document document = base_document("clamp");
  auto& page = (*document.mutable_pages())[1];
  page.set_page_no(1);
  page.mutable_size()->set_width(100);
  page.mutable_size()->set_height(100);
  auto* base = text_base(&document, docv1::BaseTextItem::kText,
                         docv1::DOC_ITEM_LABEL_TEXT, "T");
  auto* prov = base->add_prov();
  prov->set_page_no(1);
  prov->mutable_bbox()->set_l(-5);
  prov->mutable_bbox()->set_t(-1);
  prov->mutable_bbox()->set_r(105.5);
  prov->mutable_bbox()->set_b(99);
  // A provenance on an unknown page stays untouched.
  auto* off_page = base->add_prov();
  off_page->set_page_no(9);
  off_page->mutable_bbox()->set_l(-7);
  off_page->mutable_bbox()->set_t(0);
  off_page->mutable_bbox()->set_r(1);
  off_page->mutable_bbox()->set_b(1);

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered, "\"l\": 0.0,\n            \"t\": 0.0,\n"
                             "            \"r\": 100.0,\n            \"b\": 99.0",
                   "out-of-page coordinates clamp to the page box");
  require_contains(rendered, "\"l\": -7.0", "unknown pages never clamp");
}

// An ordered-list group relabels to a plain list group at load, exactly like
// the reference model, and its items stay put: no migration, no synthesized
// group, and the ordered label never reaches the canonical output.
void verify_ordered_list_groups_relabel_to_list() {
  docv1::Document document = base_document("ordered");
  auto* item_base = text_base(&document, docv1::BaseTextItem::kListItem,
                              docv1::DOC_ITEM_LABEL_LIST_ITEM, "one");
  const std::string item_ref = item_base->self_ref();
  auto* body_children = document.mutable_body()->mutable_children();
  body_children->RemoveLast();  // the helper linked the item under the body
  const std::string group_ref = "#/groups/" + std::to_string(document.groups_size());
  auto* group = document.add_groups();
  group->set_self_ref(group_ref);
  group->mutable_parent()->set_ref("#/body");
  group->set_content_layer(docv1::CONTENT_LAYER_BODY);
  group->set_name("group");
  group->set_label(docv1::GROUP_LABEL_ORDERED_LIST);
  group->add_children()->set_ref(item_ref);
  body_children->Add()->set_ref(group_ref);
  item_base->mutable_parent()->set_ref(group_ref);

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered, "\"label\": \"list\"", "the group relabels to list");
  require_absent(rendered, "ordered_list", "the ordered label never dumps");
  require_contains(rendered, "\"$ref\": \"#/texts/0\"", "the item keeps its home");
  require(rendered.find("\"#/groups/1\"") == std::string::npos,
          "no group is synthesized for an ordered-list member");
}

void verify_misplaced_list_items_migrate_into_a_group() {
  docv1::Document document = base_document("migrate");
  text_base(&document, docv1::BaseTextItem::kText, docv1::DOC_ITEM_LABEL_TEXT,
            "before");
  auto* first = text_base(&document, docv1::BaseTextItem::kListItem,
                          docv1::DOC_ITEM_LABEL_LIST_ITEM, "item one");
  auto* list = document.mutable_texts(1)->mutable_list_item();
  list->set_enumerated(true);
  list->set_marker("*");
  first->mutable_meta()->mutable_summary()->set_text("left behind");
  auto* prov = first->add_prov();
  prov->set_page_no(1);
  prov->mutable_bbox()->set_l(1);
  prov->mutable_bbox()->set_t(2);
  prov->mutable_bbox()->set_r(3);
  prov->mutable_bbox()->set_b(4);
  auto* second_prov = first->add_prov();
  second_prov->set_page_no(1);
  second_prov->mutable_bbox()->set_l(5);
  second_prov->mutable_bbox()->set_t(6);
  second_prov->mutable_bbox()->set_r(7);
  second_prov->mutable_bbox()->set_b(8);
  text_base(&document, docv1::BaseTextItem::kListItem,
            docv1::DOC_ITEM_LABEL_LIST_ITEM, "item two");
  text_base(&document, docv1::BaseTextItem::kText, docv1::DOC_ITEM_LABEL_TEXT,
            "after");

  const std::string rendered = grparse::render_canonical_json(document);
  // The synthesized group takes the run's place; the survivors renumber and
  // the migrated items re-append at the arena's end.
  require_contains(rendered,
                   "\"children\": [\n      {\n        \"$ref\": \"#/texts/0\"\n"
                   "      },\n      {\n        \"$ref\": \"#/groups/0\"\n      },\n"
                   "      {\n        \"$ref\": \"#/texts/1\"\n      }\n    ],",
                   "the group replaces the run in the body's children");
  require_contains(rendered, "\"label\": \"list\"", "the group is a list group");
  require_contains(rendered, "\"name\": \"group\"", "the group takes the default name");
  require_contains(rendered,
                   "\"children\": [\n        {\n          \"$ref\": \"#/texts/2\"\n"
                   "        },\n        {\n          \"$ref\": \"#/texts/3\"\n        }\n      ],",
                   "migrated items hang under the group");
  require_absent(rendered, "left behind", "migration drops item metadata");
  require_contains(rendered, "\"marker\": \"*\"", "markers carry over");
  const std::size_t first_prov = rendered.find("\"l\": 1.0");
  require(first_prov != std::string::npos, "the first provenance survives");
  require_absent(rendered, "\"l\": 5.0", "later provenances are dropped");
}

// A document that needs neither normalization takes the zero-copy path;
// its output must be byte-stable across renders and show no synthesized
// group and no altered coordinates.
void verify_clean_documents_render_unnormalized() {
  docv1::Document document = base_document("clean");
  auto& page = (*document.mutable_pages())[1];
  page.set_page_no(1);
  page.mutable_size()->set_width(200);
  page.mutable_size()->set_height(200);
  auto* base = text_base(&document, docv1::BaseTextItem::kText,
                         docv1::DOC_ITEM_LABEL_TEXT, "in bounds");
  auto* prov = base->add_prov();
  prov->set_page_no(1);
  prov->mutable_bbox()->set_l(10);
  prov->mutable_bbox()->set_t(20);
  prov->mutable_bbox()->set_r(30);
  prov->mutable_bbox()->set_b(40);

  // A list item already parented under a list group must not migrate.
  auto* item_base = text_base(&document, docv1::BaseTextItem::kListItem,
                              docv1::DOC_ITEM_LABEL_LIST_ITEM, "grouped");
  const std::string item_ref = item_base->self_ref();
  auto* body_children = document.mutable_body()->mutable_children();
  body_children->RemoveLast();  // the helper linked the item under the body
  const std::string group_ref = "#/groups/" + std::to_string(document.groups_size());
  auto* group = document.add_groups();
  group->set_self_ref(group_ref);
  group->mutable_parent()->set_ref("#/body");
  group->set_content_layer(docv1::CONTENT_LAYER_BODY);
  group->set_name("list");
  group->set_label(docv1::GROUP_LABEL_LIST);
  group->add_children()->set_ref(item_ref);
  body_children->Add()->set_ref(group_ref);
  item_base->mutable_parent()->set_ref(group_ref);

  const std::string rendered = grparse::render_canonical_json(document);
  require_contains(rendered, "\"l\": 10.0", "in-bounds coordinates pass through");
  require_contains(rendered, "\"name\": \"list\"", "the caller's group survives");
  require_absent(rendered, "\"name\": \"group\"",
                 "no group is synthesized for a well-homed list item");
  require(rendered == grparse::render_canonical_json(document),
          "repeated renders are byte-stable");
}

// The emitter never mutates its input, so many threads may render one
// shared document concurrently on both the zero-copy and the normalizing
// path; every thread must produce the reference bytes.
void verify_concurrent_renders_share_one_document() {
  docv1::Document clean = base_document("shared-clean");
  text_base(&clean, docv1::BaseTextItem::kText, docv1::DOC_ITEM_LABEL_TEXT,
            "alpha");
  docv1::Document migrating = base_document("shared-migrate");
  text_base(&migrating, docv1::BaseTextItem::kListItem,
            docv1::DOC_ITEM_LABEL_LIST_ITEM, "beta");

  for (const docv1::Document* document : {&clean, &migrating}) {
    const std::string reference = grparse::render_canonical_json(*document);
    std::vector<std::string> results(8);
    {
      std::vector<std::jthread> pool;
      for (auto& slot : results) {
        pool.emplace_back(
            [&slot, document] { slot = grparse::render_canonical_json(*document); });
      }
    }
    for (const auto& result : results) {
      require(result == reference, "concurrent renders agree with the reference");
    }
  }
}

// Property check on the number formatter: every finite double, fed through
// its canonical text, parses back to the identical bit pattern.
void verify_double_formatting_round_trips() {
  std::mt19937_64 rng(0x5eed5eed);
  int checked = 0;
  while (checked < 5000) {
    const std::uint64_t bits = rng();
    double value;
    std::memcpy(&value, &bits, sizeof value);
    if (!std::isfinite(value)) continue;
    const std::string text = grparse::render::canonical_double(value);
    const double parsed = std::strtod(text.c_str(), nullptr);
    require(parsed == value, "canonical text round-trips: " + text);
    ++checked;
  }
}

// The bulk fast path in the escaper must agree with a naive
// character-at-a-time reference on mixed content.
void verify_escape_fast_path_equivalence() {
  const std::string clean_run(4096, 'a');
  const std::vector<std::string> cases = {
      clean_run,
      clean_run + "\"" + clean_run,
      "tab\tquote\"back\\slash" + clean_run + "\xc3\xa9 end",
      std::string("\x01\x02") + clean_run + "\x7f",
      "\xf0\x9f\x8e\xb8" + clean_run,  // astral pair splits into surrogates
  };
  for (const auto& text : cases) {
    std::string reference;
    for (const char c : text) {
      grparse::render::escape_json_ascii_into(reference, std::string_view(&c, 1));
    }
    // Byte-at-a-time destroys multibyte sequences, so the reference for
    // non-ASCII cases is instead the whole-string escape recomputed through
    // the wrapper; the fast path and wrapper must agree exactly.
    const std::string whole = grparse::render::escape_json_ascii(text);
    std::string appended;
    grparse::render::escape_json_ascii_into(appended, text);
    require(appended == whole, "append and wrapper escapes agree");
    if (text.find('\xc3') == std::string::npos &&
        text.find('\xf0') == std::string::npos) {
      require(whole == reference, "fast path matches the naive reference");
    }
  }
}

}  // namespace

int main() {
  try {
    verify_double_formatting_matches_reference_repr();
    verify_integral_decimal_is_exact();
    verify_ascii_escaping_matches_reference_dump();
    verify_empty_document_layout_and_identity();
    verify_identity_header_is_constant();
    verify_text_variants_flatten_with_subclass_members();
    verify_generic_arm_dispatches_on_label();
    verify_code_item_inlined_base_flattens();
    verify_prov_charspan_and_coord_origins();
    verify_table_grid_spans_headers_and_rich_cells();
    verify_picture_meta_and_annotations();
    verify_key_value_graphs();
    verify_track_sources_and_collector_dropping();
    verify_meta_suppression_and_defaults();
    verify_custom_field_rekeying_and_values();
    verify_field_arenas_suppressed_when_empty();
    verify_pages_dump_in_numeric_order();
    verify_origin_and_raw_label_states();
    verify_bboxes_clamp_to_their_page();
    verify_ordered_list_groups_relabel_to_list();
    verify_misplaced_list_items_migrate_into_a_group();
    verify_clean_documents_render_unnormalized();
    verify_concurrent_renders_share_one_document();
    verify_double_formatting_round_trips();
    verify_escape_fast_path_equivalence();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "canonical-json-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
