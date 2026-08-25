// Covers the Docs API export: the named styles a heading maps to, list
// nesting under one identifier, spanned tables, caption adjacency, the
// inline-image placeholder contract, byte-for-byte determinism, and the
// service surface accepting and dispatching the new output format.
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse.grpc.pb.h"
#include "grparse/document_parser_service.h"
#include "grparse/document_render.h"
#include "grparse/page_scheduler.h"
#include "../src/render/gdocs_renderer.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
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
    throw std::runtime_error(message + " (present: " + needle + ")\nrendered:\n" +
                             haystack);
  }
}

// The number of times `needle` occurs in `haystack`, which is how the
// render-once assertions are stated.
int occurrences(const std::string& haystack, const std::string& needle) {
  int count = 0;
  for (std::string::size_type at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

// ---------------------------------------------------------------------------
// Document fixtures. Every item lands in the body layer unless a test moves
// it, and every helper links the item under its parent the way a collector
// does.
// ---------------------------------------------------------------------------

docv1::Document base_document(const std::string& name) {
  docv1::Document document;
  document.set_name(name);
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  return document;
}

void attach(docv1::Document* document, const std::string& parent,
            const std::string& child) {
  if (parent == "#/body") {
    document->mutable_body()->add_children()->set_ref(child);
    return;
  }
  const std::string prefix = "#/groups/";
  require(parent.starts_with(prefix),
          "test fixture parent must be #/body or a group");
  document->mutable_groups(std::stoi(parent.substr(prefix.size())))
      ->add_children()
      ->set_ref(child);
}

std::string add_text(docv1::Document* document, const std::string& parent,
                     docv1::BaseTextItem::ItemCase variant,
                     docv1::DocItemLabel label, const std::string& text,
                     int level = 0, bool enumerated = false,
                     docv1::ContentLayer layer = docv1::CONTENT_LAYER_BODY) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* item = document->add_texts();
  docv1::TextItemBase* base = nullptr;
  switch (variant) {
    case docv1::BaseTextItem::kTitle: base = item->mutable_title()->mutable_base(); break;
    case docv1::BaseTextItem::kSectionHeader: {
      auto* header = item->mutable_section_header();
      header->set_level(level);
      base = header->mutable_base();
      break;
    }
    case docv1::BaseTextItem::kListItem: {
      auto* list_item = item->mutable_list_item();
      list_item->set_enumerated(enumerated);
      base = list_item->mutable_base();
      break;
    }
    default: base = item->mutable_text()->mutable_base(); break;
  }
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref(parent);
  base->set_label(label);
  base->set_content_layer(layer);
  base->set_text(text);
  attach(document, parent, ref);
  return ref;
}

std::string add_code(docv1::Document* document, const std::string& parent,
                     const std::string& text) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* code = document->add_texts()->mutable_code();
  code->set_self_ref(ref);
  code->mutable_parent()->set_ref(parent);
  code->set_label(docv1::DOC_ITEM_LABEL_CODE);
  code->set_content_layer(docv1::CONTENT_LAYER_BODY);
  code->set_text(text);
  attach(document, parent, ref);
  return ref;
}

std::string add_group(docv1::Document* document, const std::string& parent,
                      docv1::GroupLabel label) {
  const std::string ref = "#/groups/" + std::to_string(document->groups_size());
  auto* group = document->add_groups();
  group->set_self_ref(ref);
  group->mutable_parent()->set_ref(parent);
  group->set_label(label);
  group->set_content_layer(docv1::CONTENT_LAYER_BODY);
  attach(document, parent, ref);
  return ref;
}

// A caption item lives in the text arena with its float as the parent and is
// reached only through that float's captions list.
std::string add_caption(docv1::Document* document, const std::string& owner,
                        const std::string& text) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* base = document->add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref(owner);
  base->set_label(docv1::DOC_ITEM_LABEL_CAPTION);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  return ref;
}

docv1::TableItem* add_table(docv1::Document* document, const std::string& parent) {
  const std::string ref = "#/tables/" + std::to_string(document->tables_size());
  auto* table = document->add_tables();
  table->set_self_ref(ref);
  table->mutable_parent()->set_ref(parent);
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  table->set_content_layer(docv1::CONTENT_LAYER_BODY);
  attach(document, parent, ref);
  return table;
}

// Writes one cell into the flat cell list the grid materializer reads.
void add_cell(docv1::TableData* data, const std::string& text, int row, int col,
              int row_span = 1, int col_span = 1) {
  auto* cell = data->add_table_cells();
  cell->set_text(text);
  cell->set_row_span(row_span);
  cell->set_col_span(col_span);
  cell->set_start_row_offset_idx(row);
  cell->set_end_row_offset_idx(row + row_span);
  cell->set_start_col_offset_idx(col);
  cell->set_end_col_offset_idx(col + col_span);
}

docv1::PictureItem* add_picture(docv1::Document* document, const std::string& parent) {
  const std::string ref = "#/pictures/" + std::to_string(document->pictures_size());
  auto* picture = document->add_pictures();
  picture->set_self_ref(ref);
  picture->mutable_parent()->set_ref(parent);
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  picture->set_content_layer(docv1::CONTENT_LAYER_BODY);
  attach(document, parent, ref);
  return picture;
}

// A heading over a table whose header cell spans both columns: the smallest
// document that exercises the two structural elements the export emits.
docv1::Document heading_and_table_document() {
  docv1::Document document = base_document("fuel.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kSectionHeader,
           docv1::DOC_ITEM_LABEL_SECTION_HEADER, "Fuel", 1);
  auto* table = add_table(&document, "#/body");
  table->add_captions()->set_ref(add_caption(&document, table->self_ref(), "Rates"));
  auto* data = table->mutable_data();
  data->set_num_rows(2);
  data->set_num_cols(2);
  add_cell(data, "Rates", 0, 0, 1, 2);
  add_cell(data, "Diesel", 1, 0);
  add_cell(data, "0.9", 1, 1);
  return document;
}

// ---------------------------------------------------------------------------
// Renderer tests.
// ---------------------------------------------------------------------------

// The heading mapping is the seam's, so it is pinned there first and then in
// the rendered document; the API's TITLE style leaves the first heading rank
// free, so level 1 lands on HEADING_1 and everything past the last rank
// clamps onto HEADING_6.
void verify_heading_styles_map_and_clamp() {
  require(grparse::render::gdocs_heading_style(-3) == "HEADING_1" &&
              grparse::render::gdocs_heading_style(0) == "HEADING_1" &&
              grparse::render::gdocs_heading_style(1) == "HEADING_1" &&
              grparse::render::gdocs_heading_style(4) == "HEADING_4" &&
              grparse::render::gdocs_heading_style(6) == "HEADING_6" &&
              grparse::render::gdocs_heading_style(11) == "HEADING_6",
          "heading levels must map into the API's six ranks");

  docv1::Document document = base_document("levels.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kTitle,
           docv1::DOC_ITEM_LABEL_TITLE, "Handbook");
  add_text(&document, "#/body", docv1::BaseTextItem::kSectionHeader,
           docv1::DOC_ITEM_LABEL_SECTION_HEADER, "Chapter", 1);
  add_text(&document, "#/body", docv1::BaseTextItem::kSectionHeader,
           docv1::DOC_ITEM_LABEL_SECTION_HEADER, "Deep", 9);
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "Body prose.");

  const std::string rendered = grparse::render_gdocs_json(document);
  require_contains(rendered, "\"title\": \"Handbook\"",
                   "the first title item names the document");
  require_contains(rendered, "\"namedStyleType\": \"TITLE\"",
                   "the title item keeps rendering inside the body");
  require_contains(rendered, "\"namedStyleType\": \"HEADING_1\"",
                   "a level-1 section header takes the first heading rank");
  require_contains(rendered, "\"namedStyleType\": \"HEADING_6\"",
                   "a section header past the last rank clamps onto it");
  require_absent(rendered, "\"HEADING_9\"", "there is no ninth heading rank");
  require_contains(rendered, "\"namedStyleType\": \"NORMAL_TEXT\"",
                   "prose takes the normal style");
}

// A document with no title item falls back to its name, and the body then
// carries no TITLE paragraph at all.
void verify_document_name_is_the_title_fallback() {
  docv1::Document document = base_document("untitled.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "Body prose.");
  const std::string rendered = grparse::render_gdocs_json(document);
  require_contains(rendered, "\"title\": \"untitled.pdf\"",
                   "a document without a title item falls back to its name");
  require_absent(rendered, "\"TITLE\"", "no title item means no TITLE paragraph");
}

// A sublist shares its parent's identifier and steps the nesting level, which
// is how the API models nesting; the list registry declares one glyph per
// level the list reached.
void verify_list_nesting_shares_one_list_id() {
  docv1::Document document = base_document("lists.pdf");
  const std::string list = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_text(&document, list, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "alpha");
  const std::string nested = add_group(&document, list, docv1::GROUP_LABEL_ORDERED_LIST);
  add_text(&document, nested, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "one", 0, true);
  const std::string deeper = add_group(&document, nested, docv1::GROUP_LABEL_LIST);
  add_text(&document, deeper, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "deep");
  // A second top-level list opens an identifier of its own.
  const std::string second = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_text(&document, second, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "gamma");

  const std::string rendered = grparse::render_gdocs_json(document);
  require(occurrences(rendered, "\"listId\": \"kix.list0\"") == 3,
          "the whole nested list belongs to one identifier:\n" + rendered);
  require(occurrences(rendered, "\"listId\": \"kix.list1\"") == 1,
          "a second top-level list opens a second identifier:\n" + rendered);
  require_contains(rendered, "\"nestingLevel\": 0", "the outer items sit at level 0");
  require_contains(rendered, "\"nestingLevel\": 1", "a sublist steps one level");
  require_contains(rendered, "\"nestingLevel\": 2", "a sub-sublist steps again");
  // Levels 0 and 2 are bulleted, level 1 enumerates: the registry declares
  // exactly that, in level order.
  require_contains(rendered,
                   "\"nestingLevels\": [\n"
                   "          {\n"
                   "            \"glyphSymbol\": \"\\u25cf\"\n"
                   "          },\n"
                   "          {\n"
                   "            \"glyphType\": \"DECIMAL\"\n"
                   "          },\n"
                   "          {\n"
                   "            \"glyphSymbol\": \"\\u25cf\"\n"
                   "          }\n"
                   "        ]",
                   "the list registry declares one glyph per level reached");
}

// A list item the producer left outside any list group still reads as a
// one-item list, the way it does in the HTML export.
void verify_stray_list_item_opens_its_own_list() {
  docv1::Document document = base_document("stray.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "orphan", 0, true);
  const std::string rendered = grparse::render_gdocs_json(document);
  require_contains(rendered, "\"listId\": \"kix.list0\"",
                   "a stray list item opens a list of its own");
  require_contains(rendered, "\"glyphType\": \"DECIMAL\"",
                   "an enumerated stray item numbers its level");
}

// A spanning cell is written once, at the first position it covers, and the
// positions it covers are simply absent from the row, which is how the API
// expresses a span.
void verify_spanned_table_writes_each_cell_once() {
  docv1::Document document = base_document("spans.pdf");
  auto* table = add_table(&document, "#/body");
  auto* data = table->mutable_data();
  data->set_num_rows(3);
  data->set_num_cols(3);
  add_cell(data, "wide", 0, 0, 1, 3);
  add_cell(data, "tall", 1, 0, 2, 1);
  add_cell(data, "b", 1, 1);
  add_cell(data, "c", 1, 2);
  add_cell(data, "e", 2, 1);
  // (2, 2) is left uncovered on purpose: a hole renders as an empty cell.

  const std::string rendered = grparse::render_gdocs_json(document);
  require_contains(rendered, "\"rows\": 3", "the table declares its row count");
  require_contains(rendered, "\"columns\": 3", "the table declares its column count");
  require(occurrences(rendered, "\"content\": \"wide\\n\"") == 1,
          "a column-spanning cell is written once:\n" + rendered);
  require(occurrences(rendered, "\"content\": \"tall\\n\"") == 1,
          "a row-spanning cell is written once:\n" + rendered);
  require_contains(rendered, "\"columnSpan\": 3", "the column span rides the cell style");
  require_contains(rendered, "\"rowSpan\": 2", "the row span rides the cell style");
  // Row 0 holds the one spanning cell, row 1 three cells, row 2 the
  // continuation-free remainder: one declared cell and one hole.
  require(occurrences(rendered, "\"tableCellStyle\"") == 6,
          "covered positions must not be written again:\n" + rendered);
  require(occurrences(rendered, "\"content\": \"\\n\"") == 1,
          "a position no cell reaches renders as one empty cell:\n" + rendered);
}

// A caption reached through its float renders beside that float and never a
// second time, even when the tree also links it under the body.
void verify_captions_render_beside_their_float_once() {
  docv1::Document document = base_document("captions.pdf");
  auto* table = add_table(&document, "#/body");
  auto* data = table->mutable_data();
  data->set_num_rows(1);
  data->set_num_cols(1);
  add_cell(data, "cell", 0, 0);
  const std::string caption = add_caption(&document, table->self_ref(), "Table one");
  table->add_captions()->set_ref(caption);
  // The producer also linked the caption straight under the body, after the
  // table: claiming it must keep the walk from rendering it again.
  document.mutable_body()->add_children()->set_ref(caption);

  auto* picture = add_picture(&document, "#/body");
  picture->add_captions()->set_ref(
      add_caption(&document, picture->self_ref(), "Figure one"));

  const std::string rendered = grparse::render_gdocs_json(document);
  require(occurrences(rendered, "\"content\": \"Table one\\n\"") == 1,
          "a claimed caption renders exactly once:\n" + rendered);
  require(occurrences(rendered, "\"content\": \"Figure one\\n\"") == 1,
          "a figure caption renders exactly once:\n" + rendered);
  // The table's caption paragraph sits immediately ahead of the table it
  // belongs to; the figure's caption is the placeholder paragraph itself.
  const auto caption_at = rendered.find("\"content\": \"Table one\\n\"");
  const auto table_at = rendered.find("\"table\": {");
  require(caption_at != std::string::npos && table_at != std::string::npos &&
              caption_at < table_at,
          "the table caption must sit next to its table:\n" + rendered);
}

// Pictures leave a paragraph behind and a placeholder entry describing the
// upload the integration layer still owes.
void verify_picture_placeholders_carry_the_upload_contract() {
  docv1::Document document = base_document("figures.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "Before the figure.");
  auto* figure = add_picture(&document, "#/body");
  figure->add_captions()->set_ref(add_caption(&document, figure->self_ref(), "A chart"));
  auto* image = figure->mutable_image();
  image->set_mimetype("image/png");
  image->set_uri("data:image/png;base64,SEVMTE8=");
  image->mutable_size()->set_width(640);
  image->mutable_size()->set_height(480);
  // A picture with no image at all still owes an entry, zeroed.
  add_picture(&document, "#/body");

  const std::string rendered = grparse::render_gdocs_json(document);
  require_absent(rendered, "SEVMTE8=",
                 "a create body must never carry the picture bytes");
  require_contains(rendered,
                   "\"inlineImagePlaceholders\": [\n"
                   "    {\n"
                   "      \"selfRef\": \"#/pictures/0\",\n"
                   "      \"mimeType\": \"image/png\",\n"
                   "      \"size\": {\n"
                   "        \"width\": 640.0,\n"
                   "        \"height\": 480.0\n"
                   "      },\n"
                   "      \"contentOrdinal\": 1\n"
                   "    },\n"
                   "    {\n"
                   "      \"selfRef\": \"#/pictures/1\",\n"
                   "      \"mimeType\": \"\",\n"
                   "      \"size\": {\n"
                   "        \"width\": 0.0,\n"
                   "        \"height\": 0.0\n"
                   "      },\n"
                   "      \"contentOrdinal\": 2\n"
                   "    }\n"
                   "  ]",
                   "every picture owes one placeholder entry");
  require_contains(rendered, "\"content\": \"A chart\\n\"",
                   "the placeholder paragraph carries the caption text");
}

// Code rides a normal paragraph in a monospace run, and its line breaks fold
// into the soft break the API reads inside one paragraph.
void verify_code_rides_a_monospace_run() {
  docv1::Document document = base_document("code.pdf");
  add_code(&document, "#/body", "int main() {\n  return 0;\n}");
  const std::string rendered = grparse::render_gdocs_json(document);
  require_contains(rendered, "\"weightedFontFamily\": {",
                   "code carries a weighted font family");
  require_contains(rendered, "\"fontFamily\": \"Courier New\"",
                   "code takes the monospace face");
  require_contains(rendered, "\"weight\": 400", "the monospace run stays regular weight");
  require_contains(rendered,
                   "\"content\": \"int main() {\\u000b  return 0;\\u000b}\\n\"",
                   "an item's newlines fold into the API's soft break");
  require_contains(rendered, "\"namedStyleType\": \"NORMAL_TEXT\"",
                   "code stays a normal-style paragraph");
  require(grparse::render::gdocs_run_content("a\r\nb") == "a\vb\n",
          "the run folder drops carriage returns and terminates the run");
}

// The furniture layer, and every layer but the body layer, stays out of the
// export exactly as it does in the other renderers.
void verify_non_body_layers_are_skipped() {
  docv1::Document document = base_document("layers.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_PAGE_HEADER, "Running head", 0, false,
           docv1::CONTENT_LAYER_FURNITURE);
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "Body prose.");
  const std::string rendered = grparse::render_gdocs_json(document);
  require_absent(rendered, "Running head", "furniture stays out of the export");
  require_contains(rendered, "Body prose.", "body content still renders");
}

// The whole shape, pinned on the smallest document that carries both
// structural elements. This is the payload an integration hands to a create
// call, so its key order is part of the contract.
void verify_heading_and_table_document_shape() {
  const std::string rendered = grparse::render_gdocs_json(heading_and_table_document());
  const std::string expected =
      "{\n"
      "  \"title\": \"fuel.pdf\",\n"
      "  \"body\": {\n"
      "    \"content\": [\n"
      "      {\n"
      "        \"paragraph\": {\n"
      "          \"elements\": [\n"
      "            {\n"
      "              \"textRun\": {\n"
      "                \"content\": \"Fuel\\n\",\n"
      "                \"textStyle\": {}\n"
      "              }\n"
      "            }\n"
      "          ],\n"
      "          \"paragraphStyle\": {\n"
      "            \"namedStyleType\": \"HEADING_1\"\n"
      "          }\n"
      "        }\n"
      "      },\n"
      "      {\n"
      "        \"paragraph\": {\n"
      "          \"elements\": [\n"
      "            {\n"
      "              \"textRun\": {\n"
      "                \"content\": \"Rates\\n\",\n"
      "                \"textStyle\": {}\n"
      "              }\n"
      "            }\n"
      "          ],\n"
      "          \"paragraphStyle\": {\n"
      "            \"namedStyleType\": \"NORMAL_TEXT\"\n"
      "          }\n"
      "        }\n"
      "      },\n"
      "      {\n"
      "        \"table\": {\n"
      "          \"rows\": 2,\n"
      "          \"columns\": 2,\n"
      "          \"tableRows\": [\n"
      "            {\n"
      "              \"tableCells\": [\n"
      "                {\n"
      "                  \"content\": [\n"
      "                    {\n"
      "                      \"paragraph\": {\n"
      "                        \"elements\": [\n"
      "                          {\n"
      "                            \"textRun\": {\n"
      "                              \"content\": \"Rates\\n\",\n"
      "                              \"textStyle\": {}\n"
      "                            }\n"
      "                          }\n"
      "                        ],\n"
      "                        \"paragraphStyle\": {\n"
      "                          \"namedStyleType\": \"NORMAL_TEXT\"\n"
      "                        }\n"
      "                      }\n"
      "                    }\n"
      "                  ],\n"
      "                  \"tableCellStyle\": {\n"
      "                    \"rowSpan\": 1,\n"
      "                    \"columnSpan\": 2\n"
      "                  }\n"
      "                }\n"
      "              ]\n"
      "            },\n"
      "            {\n"
      "              \"tableCells\": [\n"
      "                {\n"
      "                  \"content\": [\n"
      "                    {\n"
      "                      \"paragraph\": {\n"
      "                        \"elements\": [\n"
      "                          {\n"
      "                            \"textRun\": {\n"
      "                              \"content\": \"Diesel\\n\",\n"
      "                              \"textStyle\": {}\n"
      "                            }\n"
      "                          }\n"
      "                        ],\n"
      "                        \"paragraphStyle\": {\n"
      "                          \"namedStyleType\": \"NORMAL_TEXT\"\n"
      "                        }\n"
      "                      }\n"
      "                    }\n"
      "                  ],\n"
      "                  \"tableCellStyle\": {\n"
      "                    \"rowSpan\": 1,\n"
      "                    \"columnSpan\": 1\n"
      "                  }\n"
      "                },\n"
      "                {\n"
      "                  \"content\": [\n"
      "                    {\n"
      "                      \"paragraph\": {\n"
      "                        \"elements\": [\n"
      "                          {\n"
      "                            \"textRun\": {\n"
      "                              \"content\": \"0.9\\n\",\n"
      "                              \"textStyle\": {}\n"
      "                            }\n"
      "                          }\n"
      "                        ],\n"
      "                        \"paragraphStyle\": {\n"
      "                          \"namedStyleType\": \"NORMAL_TEXT\"\n"
      "                        }\n"
      "                      }\n"
      "                    }\n"
      "                  ],\n"
      "                  \"tableCellStyle\": {\n"
      "                    \"rowSpan\": 1,\n"
      "                    \"columnSpan\": 1\n"
      "                  }\n"
      "                }\n"
      "              ]\n"
      "            }\n"
      "          ]\n"
      "        }\n"
      "      }\n"
      "    ]\n"
      "  },\n"
      "  \"lists\": {},\n"
      "  \"inlineImagePlaceholders\": []\n"
      "}";
  require(rendered == expected, "the export shape differs:\n" + rendered);
}

// Nothing in the walk reads an unordered container, so the same document
// renders byte for byte the same however often it is asked for.
void verify_render_is_deterministic() {
  docv1::Document document = heading_and_table_document();
  add_text(&document, "#/body", docv1::BaseTextItem::kTitle,
           docv1::DOC_ITEM_LABEL_TITLE, "Fuel Report");
  const std::string list = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_text(&document, list, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "alpha");
  auto* figure = add_picture(&document, "#/body");
  figure->mutable_image()->set_mimetype("image/jpeg");
  add_code(&document, "#/body", "print('hi')");

  const std::string first = grparse::render_gdocs_json(document);
  require(first == grparse::render_gdocs_json(document),
          "two renders of one document must be byte-identical");
}

// ---------------------------------------------------------------------------
// Service surface: the new format has to pass validation and reach the
// exports message the response carries.
// ---------------------------------------------------------------------------

// One blank page, which is all the dispatch test needs from a source.
class FakeSource final : public grparse::PageSource {
 public:
  int page_count() const override { return 1; }
  std::optional<grparse::OcrPage> extract_digital_page(int) const override {
    return std::nullopt;
  }
  cv::Mat render_page(int) const override {
    return cv::Mat(1, 1, CV_8UC1, cv::Scalar(1)).clone();
  }
};

class FakeRecognizer final : public grparse::PageRecognizer {
 public:
  grparse::OcrPage extract_page(const cv::Mat&) override {
    return {100, 200, {{"one", {{1, 2}, {20, 2}, {20, 12}, {1, 12}}}}};
  }
};

class TestServer final {
 public:
  TestServer()
      : scheduler_(recognizer_, {},
                   [](std::shared_ptr<const std::string>, bool, double) {
                     return std::make_shared<FakeSource>();
                   }),
        service_(scheduler_,
                 std::make_shared<grparse::CollectorEndpoints>(grparse::CollectorTargets{})) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    if (!server_ || port_ == 0) throw std::runtime_error("test server failed to start");
  }

  ~TestServer() {
    server_->Shutdown(std::chrono::system_clock::now() + 2s);
    server_->Wait();
  }

  std::unique_ptr<parsev1::ParseService::Stub> stub() const {
    return parsev1::ParseService::NewStub(grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port_), grpc::InsecureChannelCredentials()));
  }

 private:
  FakeRecognizer recognizer_;
  grparse::PageScheduler scheduler_;
  grparse::DocumentParserService service_;
  int port_ = 0;
  std::unique_ptr<grpc::Server> server_;
};

void verify_service_accepts_and_dispatches_the_format() {
  TestServer server;
  auto client = server.stub();
  parsev1::ConvertSourceRequest request;
  auto* source = request.mutable_request()->add_sources()->mutable_file();
  source->set_filename("image.png");
  source->set_base64_string("bWVtb3J5");
  request.mutable_request()->mutable_options()->add_to_formats(
      parsev1::OUTPUT_FORMAT_GDOCS_JSON);

  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 10s);
  parsev1::ConvertSourceResponse response;
  const grpc::Status status = client->ConvertSource(&context, request, &response);
  require(status.ok(),
          "the new output format must pass validation: " + status.error_message());

  const auto& exports = response.response().document().exports();
  require(exports.has_gdocs_json(),
          "requesting the format must fill the gdocs_json export");
  require_contains(exports.gdocs_json(), "\"inlineImagePlaceholders\"",
                   "the dispatched export is the Docs API body shape");
  require(!exports.has_md() && !exports.has_html() && !exports.has_canonical_json(),
          "an explicit format list renders nothing else");
}

}  // namespace

int main() {
  try {
    verify_heading_styles_map_and_clamp();
    verify_document_name_is_the_title_fallback();
    verify_list_nesting_shares_one_list_id();
    verify_stray_list_item_opens_its_own_list();
    verify_spanned_table_writes_each_cell_once();
    verify_captions_render_beside_their_float_once();
    verify_picture_placeholders_carry_the_upload_contract();
    verify_code_rides_a_monospace_run();
    verify_non_body_layers_are_skipped();
    verify_heading_and_table_document_shape();
    verify_render_is_deterministic();
    verify_service_accepts_and_dispatches_the_format();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "gdocs-renderer-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
