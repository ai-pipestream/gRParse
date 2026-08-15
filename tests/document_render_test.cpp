#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include <google/protobuf/util/json_util.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_contains(const std::string& haystack, const std::string& needle,
                      const std::string& message) {
  if (haystack.find(needle) == std::string::npos) {
    throw std::runtime_error(message + " (missing: " + needle + ")\nrendered:\n" +
                             haystack);
  }
}

docv1::Document base_document(const std::string& name) {
  docv1::Document document;
  document.set_name(name);
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  return document;
}

// Links a child reference under its parent: the body root or a group.
void attach(docv1::Document* document, const std::string& parent,
            const std::string& child) {
  if (parent == "#/body") {
    document->mutable_body()->add_children()->set_ref(child);
    return;
  }
  const std::string prefix = "#/groups/";
  require(parent.compare(0, prefix.size(), prefix) == 0,
          "test fixture parent must be #/body or a group");
  document->mutable_groups(std::stoi(parent.substr(prefix.size())))
      ->add_children()
      ->set_ref(child);
}

// Appends one text arena entry of the requested variant and links it under
// the parent. Returns the new item's reference.
std::string add_text(docv1::Document* document, const std::string& parent,
                     docv1::BaseTextItem::ItemCase variant,
                     docv1::DocItemLabel label, const std::string& text,
                     int level = 0, bool enumerated = false) {
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
    case docv1::BaseTextItem::kFormula: base = item->mutable_formula()->mutable_base(); break;
    default: base = item->mutable_text()->mutable_base(); break;
  }
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref(parent);
  base->set_label(label);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  attach(document, parent, ref);
  return ref;
}

std::string add_code(docv1::Document* document, const std::string& parent,
                     const std::string& text, docv1::CodeLanguageLabel language) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* code = document->add_texts()->mutable_code();
  code->set_self_ref(ref);
  code->mutable_parent()->set_ref(parent);
  code->set_label(docv1::DOC_ITEM_LABEL_CODE);
  code->set_content_layer(docv1::CONTENT_LAYER_BODY);
  code->set_text(text);
  code->set_code_language(language);
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

// A caption text item lives in the text arena with the owning table or
// figure as its parent; it is referenced from the owner's captions list,
// never linked under the body.
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

docv1::TableCell* grid_cell(docv1::TableRow* row, const std::string& text,
                            bool column_header, int row_index, int col_index,
                            int row_span = 1, int col_span = 1) {
  auto* cell = row->add_cells();
  cell->set_text(text);
  cell->set_column_header(column_header);
  cell->set_row_span(row_span);
  cell->set_col_span(col_span);
  cell->set_start_row_offset_idx(row_index);
  cell->set_end_row_offset_idx(row_index + row_span);
  cell->set_start_col_offset_idx(col_index);
  cell->set_end_col_offset_idx(col_index + col_span);
  return cell;
}

docv1::PictureItem* add_picture(docv1::Document* document, const std::string& parent,
                                const std::string& uri) {
  const std::string ref = "#/pictures/" + std::to_string(document->pictures_size());
  auto* picture = document->add_pictures();
  picture->set_self_ref(ref);
  picture->mutable_parent()->set_ref(parent);
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  picture->set_content_layer(docv1::CONTENT_LAYER_BODY);
  if (!uri.empty()) picture->mutable_image()->set_uri(uri);
  attach(document, parent, ref);
  return picture;
}

// The document every renderer test folds: one of every item vocabulary the
// renderers handle, in a fixed body order.
docv1::Document rich_document() {
  docv1::Document document = base_document("report.pdf");

  add_text(&document, "#/body", docv1::BaseTextItem::kTitle,
           docv1::DOC_ITEM_LABEL_TITLE, "Quarterly Report");
  add_text(&document, "#/body", docv1::BaseTextItem::kSectionHeader,
           docv1::DOC_ITEM_LABEL_SECTION_HEADER, "Overview", 1);
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "Plain body text.");

  const std::string list = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_text(&document, list, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "alpha");
  add_text(&document, list, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "beta");
  const std::string nested =
      add_group(&document, list, docv1::GROUP_LABEL_ORDERED_LIST);
  add_text(&document, nested, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "one", 0, true);
  add_text(&document, nested, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "two", 0, true);

  auto* table = add_table(&document, "#/body");
  table->add_captions()->set_ref(add_caption(&document, table->self_ref(), "Fuel table"));
  auto* data = table->mutable_data();
  data->set_num_rows(2);
  data->set_num_cols(2);
  grid_cell(data->add_grid(), "Fuel", true, 0, 0);
  grid_cell(data->mutable_grid(0), "Rate|Unit", true, 0, 1);
  grid_cell(data->add_grid(), "Diesel", false, 1, 0);
  grid_cell(data->mutable_grid(1), "0.9", false, 1, 1);

  add_code(&document, "#/body", "print('hi')", docv1::CODE_LANGUAGE_LABEL_PYTHON);
  add_text(&document, "#/body", docv1::BaseTextItem::kFormula,
           docv1::DOC_ITEM_LABEL_FORMULA, "E = mc^2");
  add_text(&document, "#/body", docv1::BaseTextItem::kFormula,
           docv1::DOC_ITEM_LABEL_FORMULA, "");

  auto* figure = add_picture(&document, "#/body", "figure1.png");
  figure->add_captions()->set_ref(add_caption(&document, figure->self_ref(), "A chart"));
  add_picture(&document, "#/body", "");

  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED, "Done");

  const std::string key_value_ref = "#/key_value_items/0";
  auto* key_value = document.add_key_value_items();
  key_value->set_self_ref(key_value_ref);
  key_value->mutable_parent()->set_ref("#/body");
  key_value->set_label(docv1::DOC_ITEM_LABEL_KEY_VALUE_REGION);
  document.mutable_body()->add_children()->set_ref(key_value_ref);

  return document;
}

void verify_markdown_renders_every_item_type() {
  const std::string markdown = grparse::render_markdown(rich_document());
  const std::string expected =
      "# Quarterly Report\n"
      "\n"
      "## Overview\n"
      "\n"
      "Plain body text.\n"
      "\n"
      "- alpha\n"
      "- beta\n"
      "    1. one\n"
      "    2. two\n"
      "\n"
      "*Fuel table*\n"
      "\n"
      "| Fuel | Rate\\|Unit |\n"
      "|---|---|\n"
      "| Diesel | 0.9 |\n"
      "\n"
      "```python\nprint('hi')\n```\n"
      "\n"
      "$$E = mc^2$$\n"
      "\n"
      "<!-- formula-not-decoded -->\n"
      "\n"
      "*A chart*\n"
      "\n"
      "![A chart](figure1.png)\n"
      "\n"
      "<!-- image -->\n"
      "\n"
      "- [x] Done\n"
      "\n"
      "<!-- missing-key-value-item -->";
  require(markdown == expected,
          "markdown export differs from the docling-parity expectation:\n" + markdown);
}

void verify_markdown_reconstructs_grid_from_flat_cells() {
  docv1::Document document = base_document("cells.pdf");
  auto* table = add_table(&document, "#/body");
  auto* data = table->mutable_data();
  data->set_num_rows(2);
  data->set_num_cols(2);
  // Flat cells only, deliberately out of order and with one position left
  // uncovered; the renderer must place by offsets and blank the hole.
  auto* south_east = data->add_table_cells();
  south_east->set_text("d");
  south_east->set_start_row_offset_idx(1);
  south_east->set_end_row_offset_idx(2);
  south_east->set_start_col_offset_idx(1);
  south_east->set_end_col_offset_idx(2);
  auto* north_west = data->add_table_cells();
  north_west->set_text("a");
  north_west->set_column_header(true);
  north_west->set_end_row_offset_idx(1);
  north_west->set_end_col_offset_idx(1);
  auto* north_east = data->add_table_cells();
  north_east->set_text("b");
  north_east->set_column_header(true);
  north_east->set_start_col_offset_idx(1);
  north_east->set_end_row_offset_idx(1);
  north_east->set_end_col_offset_idx(2);

  const std::string markdown = grparse::render_markdown(document);
  require(markdown == "| a | b |\n|---|---|\n|  | d |",
          "flat-cell table reconstruction differs:\n" + markdown);
}

void verify_markdown_multiline_cells_stay_single_line() {
  docv1::Document document = base_document("multiline.pdf");
  auto* table = add_table(&document, "#/body");
  auto* data = table->mutable_data();
  data->set_num_rows(1);
  data->set_num_cols(1);
  grid_cell(data->add_grid(), "first\nsecond", true, 0, 0);
  const std::string markdown = grparse::render_markdown(document);
  require(markdown == "| first second |\n|---|",
          "cell newlines must collapse to spaces:\n" + markdown);
}

void verify_html_renders_structure_and_escapes() {
  docv1::Document document = base_document("page & doc.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kTitle,
           docv1::DOC_ITEM_LABEL_TITLE, "T<i>tle");
  add_text(&document, "#/body", docv1::BaseTextItem::kSectionHeader,
           docv1::DOC_ITEM_LABEL_SECTION_HEADER, "Deep", 2);
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "A & B < C > D");

  const std::string list = add_group(&document, "#/body", docv1::GROUP_LABEL_ORDERED_LIST);
  add_text(&document, list, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "one", 0, true);
  const std::string nested = add_group(&document, list, docv1::GROUP_LABEL_LIST);
  add_text(&document, nested, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "sub");

  auto* table = add_table(&document, "#/body");
  table->add_captions()->set_ref(add_caption(&document, table->self_ref(), "Spans"));
  auto* data = table->mutable_data();
  data->set_num_rows(2);
  data->set_num_cols(2);
  // The header cell spans both columns; the grid mirrors it into each
  // covered position, as the table-structure and office folds do.
  auto* header_row = data->add_grid();
  grid_cell(header_row, "H", true, 0, 0, 1, 2);
  grid_cell(header_row, "H", true, 0, 0, 1, 2);
  auto* body_row = data->add_grid();
  grid_cell(body_row, "a", false, 1, 0);
  grid_cell(body_row, "b", false, 1, 1);

  auto* figure = add_picture(&document, "#/body", "figs/one.png");
  figure->add_captions()->set_ref(
      add_caption(&document, figure->self_ref(), "Says \"hi\""));
  add_picture(&document, "#/body", "");

  add_code(&document, "#/body", "<tag>&</tag>", docv1::CODE_LANGUAGE_LABEL_XML);
  add_text(&document, "#/body", docv1::BaseTextItem::kFormula,
           docv1::DOC_ITEM_LABEL_FORMULA, "x < y");

  const std::string html = grparse::render_html(document);
  require_contains(html,
                   "<!DOCTYPE html>\n<html>\n<head><meta charset=\"UTF-8\"/>"
                   "<title>page &amp; doc.pdf</title></head>\n<body>\n",
                   "html document skeleton");
  require(html.find("</body>\n</html>") == html.size() - std::string("</body>\n</html>").size(),
          "html must close the skeleton it opened:\n" + html);
  require_contains(html, "<h1>T&lt;i&gt;tle</h1>", "escaped h1 title");
  require_contains(html, "<h3>Deep</h3>", "level 2 section header maps to h3");
  require_contains(html, "<p>A &amp; B &lt; C &gt; D</p>", "escaped paragraph");
  require_contains(html, "<ol><li>one</li><li><ul><li>sub</li></ul></li></ol>",
                   "ordered list with nested unordered list");
  require_contains(html,
                   "<table><caption>Spans</caption><tbody>"
                   "<tr><th colspan=\"2\">H</th></tr>"
                   "<tr><td>a</td><td>b</td></tr></tbody></table>",
                   "table with caption, header cell, and colspan");
  require_contains(html,
                   "<figure><img src=\"figs/one.png\" alt=\"Says &quot;hi&quot;\"/>"
                   "<figcaption>Says \"hi\"</figcaption></figure>",
                   "figure with attribute-escaped alt and figcaption");
  require_contains(html, "<figure><!-- image --></figure>",
                   "figure with no reference degrades to the image placeholder");
  require_contains(html, "<pre><code>&lt;tag&gt;&amp;&lt;/tag&gt;</code></pre>",
                   "escaped code block");
  require_contains(html, "<div class=\"formula\">x &lt; y</div>", "escaped formula block");
}

void verify_captions_render_once() {
  // A caption that is both claimed by its table and linked under the body
  // must render with the table only, never as a second paragraph.
  docv1::Document document = base_document("caption.pdf");
  auto* table = add_table(&document, "#/body");
  const std::string caption_ref =
      add_caption(&document, table->self_ref(), "Only once");
  table->add_captions()->set_ref(caption_ref);
  document.mutable_body()->add_children()->set_ref(caption_ref);
  auto* data = table->mutable_data();
  data->set_num_rows(1);
  data->set_num_cols(1);
  grid_cell(data->add_grid(), "x", true, 0, 0);

  const std::string markdown = grparse::render_markdown(document);
  require(markdown == "*Only once*\n\n| x |\n|---|",
          "caption must render exactly once:\n" + markdown);
  const std::string html = grparse::render_html(document);
  require(html.find("Only once") == html.rfind("Only once"),
          "html caption must render exactly once:\n" + html);
}

void verify_empty_document_renders() {
  const docv1::Document document = base_document("empty.pdf");
  require(grparse::render_markdown(document).empty(),
          "an empty body renders empty markdown");
  require(grparse::render_html(document) ==
              "<!DOCTYPE html>\n<html>\n<head><meta charset=\"UTF-8\"/>"
              "<title>empty.pdf</title></head>\n<body>\n</body>\n</html>",
          "an empty body renders the bare html skeleton");
  const std::string json = grparse::render_json(document);
  docv1::Document parsed;
  require(google::protobuf::util::JsonStringToMessage(json, &parsed).ok(),
          "empty-document json parses back");
  require(parsed.name() == "empty.pdf", "empty-document json keeps the name");
}

void verify_json_preserves_field_names_and_round_trips() {
  const docv1::Document document = rich_document();
  const std::string json = grparse::render_json(document);
  require_contains(json, "\"self_ref\"", "json keeps proto field names");
  require(json.find("\"selfRef\"") == std::string::npos,
          "json must not use camelCase field names:\n" + json.substr(0, 200));
  docv1::Document parsed;
  require(google::protobuf::util::JsonStringToMessage(json, &parsed).ok(),
          "document json parses back into the proto");
  require(parsed.texts_size() == document.texts_size() &&
              parsed.tables_size() == document.tables_size() &&
              parsed.pictures_size() == document.pictures_size() &&
              parsed.groups_size() == document.groups_size(),
          "round-tripped json keeps every arena");
  require(parsed.SerializeAsString() == document.SerializeAsString(),
          "round-tripped json is lossless");
}

}  // namespace

int main() {
  try {
    verify_markdown_renders_every_item_type();
    verify_markdown_reconstructs_grid_from_flat_cells();
    verify_markdown_multiline_cells_stay_single_line();
    verify_html_renders_structure_and_escapes();
    verify_captions_render_once();
    verify_empty_document_renders();
    verify_json_preserves_field_names_and_round_trips();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "document-render-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
