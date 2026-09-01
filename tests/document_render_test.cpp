#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>

#include <google/protobuf/util/json_util.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

using grparse_test::require;

void require_contains(const std::string& haystack, const std::string& needle,
                      const std::string& message) {
  if (!haystack.contains(needle)) {
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
  require(parent.starts_with(prefix),
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

// Writes one cell into both projections a producer fills: the flat cell list
// the exports read and the wire grid the markup renderers read.
docv1::TableCell* grid_cell(docv1::TableData* data, docv1::TableRow* row,
                            const std::string& text, bool column_header,
                            int row_index, int col_index, int row_span = 1,
                            int col_span = 1) {
  auto* cell = row->add_cells();
  cell->set_text(text);
  cell->set_column_header(column_header);
  cell->set_row_span(row_span);
  cell->set_col_span(col_span);
  cell->set_start_row_offset_idx(row_index);
  cell->set_end_row_offset_idx(row_index + row_span);
  cell->set_start_col_offset_idx(col_index);
  cell->set_end_col_offset_idx(col_index + col_span);
  *data->add_table_cells() = *cell;
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
  grid_cell(data, data->add_grid(), "Fuel", true, 0, 0);
  grid_cell(data, data->mutable_grid(0), "Rate|Unit", true, 0, 1);
  grid_cell(data, data->add_grid(), "Diesel", false, 1, 0);
  grid_cell(data, data->mutable_grid(1), "0.9", false, 1, 1);

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
      // The nested group carries the ordered-list label, which the load
      // normalization rewrites to a plain list group exactly like the
      // reference load does: its items stay one level deep. The unset proto
      // marker means the model-default dash, so enumeration does not number
      // them (numbering requires an explicitly empty marker).
      "    - one\n"
      "    - two\n"
      "\n"
      "Fuel table\n"
      "\n"
      "| Fuel   |   Rate&#124;Unit |\n"
      "|--------|------------------|\n"
      "| Diesel |              0.9 |\n"
      "\n"
      "```\nprint('hi')\n```\n"
      "\n"
      "$$E = mc^2$$\n"
      "\n"
      "A chart\n"
      "\n"
      "<!-- image -->\n"
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
  require(markdown == "| a   | b   |\n|-----|-----|\n|     | d   |",
          "flat-cell table reconstruction differs:\n" + markdown);
}

void verify_markdown_multiline_cells_stay_single_line() {
  docv1::Document document = base_document("multiline.pdf");
  auto* table = add_table(&document, "#/body");
  auto* data = table->mutable_data();
  data->set_num_rows(1);
  data->set_num_cols(1);
  grid_cell(data, data->add_grid(), "first\nsecond", true, 0, 0);
  const std::string markdown = grparse::render_markdown(document);
  require(markdown == "| first second   |\n|----------------|",
          "cell newlines must collapse to spaces:\n" + markdown);
}

// The rules with the least margin for error: what escapes, what the
// formatting delimiters look like and in what order they nest, and how a list
// picks its marker and its indent. Every expectation here is the reference
// rendering of the same document, taken from the validation harness
// (scripts/validate_markdown.py).
void verify_markdown_escaping_marker_and_formatting_rules() {
  docv1::Document document = base_document("pins.pdf");

  const auto text_base = [&](const std::string& ref) -> docv1::TextItemBase* {
    const std::string prefix = "#/texts/";
    return document.mutable_texts(std::stoi(ref.substr(prefix.size())))
        ->mutable_text()
        ->mutable_base();
  };
  const auto list_item = [&](const std::string& ref) -> docv1::ListItem* {
    const std::string prefix = "#/texts/";
    return document.mutable_texts(std::stoi(ref.substr(prefix.size())))
        ->mutable_list_item();
  };

  // Underscores escape and the HTML specials escape, but an inline image
  // target is left verbatim so the URL survives.
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "snake_case and <b>a & b</b>");
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "see ![alt_text](http://x/a_b.png) here");

  // Formatting wraps the escaped text, bold innermost; underline and the
  // scripts have no Markdown spelling and drop.
  const std::string bold = add_text(&document, "#/body", docv1::BaseTextItem::kText,
                                    docv1::DOC_ITEM_LABEL_TEXT, "bold_text");
  text_base(bold)->mutable_formatting()->set_bold(true);
  const std::string both = add_text(&document, "#/body", docv1::BaseTextItem::kText,
                                    docv1::DOC_ITEM_LABEL_TEXT, "both");
  text_base(both)->mutable_formatting()->set_bold(true);
  text_base(both)->mutable_formatting()->set_italic(true);
  const std::string marks = add_text(&document, "#/body", docv1::BaseTextItem::kText,
                                     docv1::DOC_ITEM_LABEL_TEXT, "marks");
  text_base(marks)->mutable_formatting()->set_strikethrough(true);
  text_base(marks)->mutable_formatting()->set_underline(true);
  text_base(marks)->mutable_formatting()->set_script(docv1::SCRIPT_SUPER);

  // The hyperlink wraps last and its target is normalized, not escaped.
  const std::string link = add_text(&document, "#/body", docv1::BaseTextItem::kText,
                                    docv1::DOC_ITEM_LABEL_TEXT, "link_here");
  text_base(link)->set_hyperlink("https://EXAMPLE.com");

  // An already-valid marker stays as the whole marker; any other marker with
  // an alphanumeric in it rides behind a generated "-"; an item with no
  // marker of its own takes the model default "-". A nested group indents by
  // four spaces per level.
  const std::string list = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  const std::string starred = add_text(&document, list, docv1::BaseTextItem::kListItem,
                                       docv1::DOC_ITEM_LABEL_LIST_ITEM, "starred");
  list_item(starred)->set_marker("*");
  const std::string lettered = add_text(&document, list, docv1::BaseTextItem::kListItem,
                                        docv1::DOC_ITEM_LABEL_LIST_ITEM, "lettered");
  list_item(lettered)->set_marker("a)");
  add_text(&document, list, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "bulleted");
  const std::string sub = add_group(&document, list, docv1::GROUP_LABEL_LIST);
  add_text(&document, sub, docv1::BaseTextItem::kListItem,
           docv1::DOC_ITEM_LABEL_LIST_ITEM, "deeper");

  // An enumerated item whose marker is present but empty is numbered by its
  // position in the group.
  const std::string enumerated = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  const std::string first = add_text(&document, enumerated,
                                     docv1::BaseTextItem::kListItem,
                                     docv1::DOC_ITEM_LABEL_LIST_ITEM, "first", 0, true);
  list_item(first)->set_marker("");
  const std::string second = add_text(&document, enumerated,
                                      docv1::BaseTextItem::kListItem,
                                      docv1::DOC_ITEM_LABEL_LIST_ITEM, "second", 0, true);
  list_item(second)->set_marker("");

  const std::string markdown = grparse::render_markdown(document);
  const std::string expected =
      "snake\\_case and &lt;b&gt;a &amp; b&lt;/b&gt;\n"
      "\n"
      "see ![alt_text](http://x/a_b.png) here\n"
      "\n"
      "**bold\\_text**\n"
      "\n"
      "***both***\n"
      "\n"
      "~~marks~~\n"
      "\n"
      "[link\\_here](https://example.com/)\n"
      "\n"
      "* starred\n"
      "- a) lettered\n"
      "- bulleted\n"
      "    - deeper\n"
      "\n"
      "1. first\n"
      "2. second";
  require(markdown == expected,
          "markdown escaping/marker/formatting rules differ:\n" + markdown);
}

// Custom meta fields render in the order the exports give them: a name
// already carrying a namespace first, then the pipestream-namespaced folds of
// the rest, with a collision between two folded names broken by a numeric
// suffix. The wire map is unordered, so this order is the export's own
// deterministic choice, shared with the canonical JSON export. A string
// inside a container is quoted and its non-printable code points spelled
// out; a string standing alone is not.
void verify_markdown_custom_meta_field_order() {
  docv1::Document document = base_document("meta-order.pdf");
  auto& fields = *document.mutable_body()->mutable_meta()->mutable_custom_fields();
  fields["A"].set_string_value("capital");
  fields["acme__note"].set_string_value("conforming");
  fields["z:one"].set_string_value("folded");
  fields["z_one"].set_string_value("collides");
  auto* list = fields["acme__list"].mutable_list_value();
  list->add_values()->set_string_value("a\xc2\xa0" "b");      // U+00A0
  list->add_values()->set_string_value("z\xe2\x80\x8b" "w");  // U+200B
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "body");

  const std::string markdown = grparse::render_markdown(document);
  require(markdown == "body\n\n['a\\xa0b', 'z\\u200bw']\n\nconforming\n\ncapital"
                      "\n\nfolded\n\ncollides",
          "custom meta field order differs:\n" + markdown);
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
  grid_cell(data, header_row, "H", true, 0, 0, 1, 2);
  header_row->add_cells()->CopyFrom(data->table_cells(0));
  auto* body_row = data->add_grid();
  grid_cell(data, body_row, "a", false, 1, 0);
  grid_cell(data, body_row, "b", false, 1, 1);

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

// Only the body layer (and the unspecified default) may render: notes and
// invisible items are deliberate non-default layers and must stay out of
// every export.
void verify_non_body_layers_are_excluded() {
  docv1::Document document = base_document("layers.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "visible body");

  // A speaker-notes text item on the notes layer.
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "speaker notes");
  document.mutable_texts(document.texts_size() - 1)
      ->mutable_text()
      ->mutable_base()
      ->set_content_layer(docv1::CONTENT_LAYER_NOTES);

  // A hidden-sheet group on the invisible layer, holding a text child.
  const std::string sheet =
      add_group(&document, "#/body", docv1::GROUP_LABEL_SHEET);
  document.mutable_groups(document.groups_size() - 1)
      ->set_content_layer(docv1::CONTENT_LAYER_INVISIBLE);
  add_text(&document, sheet, docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "hidden cell");
  document.mutable_texts(document.texts_size() - 1)
      ->mutable_text()
      ->mutable_base()
      ->set_content_layer(docv1::CONTENT_LAYER_INVISIBLE);

  // An item left on the unspecified layer renders like body content.
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "default layer");
  document.mutable_texts(document.texts_size() - 1)
      ->mutable_text()
      ->mutable_base()
      ->set_content_layer(docv1::CONTENT_LAYER_UNSPECIFIED);

  const std::string markdown = grparse::render_markdown(document);
  const std::string html = grparse::render_html(document);
  const std::string doctags = grparse::render_doctags(document);
  const std::string doclang = grparse::render_doclang(document);
  for (const auto& [name, rendered] :
       {std::pair<std::string, const std::string&>{"markdown", markdown},
        {"html", html},
        {"doctags", doctags},
        {"doclang", doclang}}) {
    require_contains(rendered, "visible body", name + " keeps body items");
    require_contains(rendered, "default layer",
                     name + " treats the unspecified layer as body");
    require(!rendered.contains("speaker notes"),
            name + " must exclude notes-layer items:\n" + rendered);
    require(!rendered.contains("hidden cell"),
            name + " must exclude invisible-layer items:\n" + rendered);
  }
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
  grid_cell(data, data->add_grid(), "x", true, 0, 0);

  const std::string markdown = grparse::render_markdown(document);
  require(markdown == "Only once\n\n| x   |\n|-----|",
          "caption must render exactly once:\n" + markdown);
  const std::string html = grparse::render_html(document);
  require(html.find("Only once") == html.rfind("Only once"),
          "html caption must render exactly once:\n" + html);
}

// A picture description must surface in the human-readable exports whether
// it rides the meta field or the annotation list, and the meta field wins
// when both are present.
void verify_picture_descriptions_surface_in_exports() {
  // Meta only.
  docv1::Document meta_only = base_document("meta.pdf");
  auto* meta_picture = add_picture(&meta_only, "#/body", "fig.png");
  meta_picture->mutable_meta()->mutable_description()->set_text(
      "A bar chart of quarterly sales");
  // Markdown positions every picture with the placeholder (the export's
  // default image mode) and prints the description as the meta paragraph
  // after it.
  const std::string meta_markdown = grparse::render_markdown(meta_only);
  require_contains(meta_markdown,
                   "<!-- image -->\n\nA bar chart of quarterly sales",
                   "markdown surfaces the meta description under the image");
  require_contains(grparse::render_html(meta_only),
                   "<figure><img src=\"fig.png\" alt=\"Image\"/>"
                   "<p>A bar chart of quarterly sales</p></figure>",
                   "html surfaces the meta description inside the figure");
  require_contains(grparse::render_doclang(meta_only),
                   "<picture uri=\"fig.png\">\n"
                   "    <description>A bar chart of quarterly sales</description>\n"
                   "  </picture>",
                   "doclang nests the meta description in the picture");

  // Annotations only.
  docv1::Document annotation_only = base_document("annotation.pdf");
  auto* annotated = add_picture(&annotation_only, "#/body", "fig.png");
  auto* annotation = annotated->add_annotations()->mutable_description();
  annotation->set_kind("description");
  annotation->set_text("An annotated diagram");
  // Markdown does not: the wire annotation list is a projection of meta that
  // the exports drop, exactly as the canonical JSON export drops it.
  const std::string annotation_markdown = grparse::render_markdown(annotation_only);
  require(annotation_markdown == "<!-- image -->",
          "markdown must ignore the wire annotation projection:\n" +
              annotation_markdown);
  require_contains(grparse::render_html(annotation_only),
                   "<p>An annotated diagram</p>",
                   "html falls back to the annotation description");
  require_contains(grparse::render_doclang(annotation_only),
                   "<description>An annotated diagram</description>",
                   "doclang falls back to the annotation description");

  // Both: the meta field wins.
  docv1::Document both = base_document("both.pdf");
  auto* both_picture = add_picture(&both, "#/body", "fig.png");
  both_picture->mutable_meta()->mutable_description()->set_text("meta text");
  both_picture->add_annotations()->mutable_description()->set_text(
      "annotation text");
  const std::string both_markdown = grparse::render_markdown(both);
  require_contains(both_markdown, "<!-- image -->\n\nmeta text",
                   "markdown prefers the meta description");
  require(!both_markdown.contains("annotation text"),
          "markdown must not also render the annotation description:\n" +
              both_markdown);

  // A caption keeps its figcaption and the description joins it as a
  // paragraph; a description on a picture with no image still renders.
  docv1::Document captioned = base_document("captioned.pdf");
  auto* figure = add_picture(&captioned, "#/body", "fig.png");
  figure->add_captions()->set_ref(
      add_caption(&captioned, figure->self_ref(), "Figure 1"));
  figure->mutable_meta()->mutable_description()->set_text("described");
  require_contains(grparse::render_html(captioned),
                   "<figcaption>Figure 1<p>described</p></figcaption>",
                   "html appends the description inside the figcaption");
  docv1::Document imageless = base_document("imageless.pdf");
  add_picture(&imageless, "#/body", "")
      ->mutable_meta()
      ->mutable_description()
      ->set_text("no image here");
  require_contains(grparse::render_markdown(imageless),
                   "<!-- image -->\n\nno image here",
                   "markdown keeps the description under the placeholder");
}

// Appends one provenance entry to the most recently added text item.
void add_prov_to_last_text(docv1::Document* document, int page_no, double l,
                           double t, double r, double b) {
  auto* item = document->mutable_texts(document->texts_size() - 1);
  docv1::TextItemBase* base = nullptr;
  switch (item->item_case()) {
    case docv1::BaseTextItem::kTitle: base = item->mutable_title()->mutable_base(); break;
    case docv1::BaseTextItem::kSectionHeader:
      base = item->mutable_section_header()->mutable_base();
      break;
    case docv1::BaseTextItem::kListItem:
      base = item->mutable_list_item()->mutable_base();
      break;
    case docv1::BaseTextItem::kFormula: base = item->mutable_formula()->mutable_base(); break;
    default: base = item->mutable_text()->mutable_base(); break;
  }
  auto* prov = base->add_prov();
  prov->set_page_no(page_no);
  auto* bbox = prov->mutable_bbox();
  bbox->set_l(l);
  bbox->set_t(t);
  bbox->set_r(r);
  bbox->set_b(b);
  bbox->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
}

void verify_doctags_renders_every_item_type() {
  const std::string doctags = grparse::render_doctags(rich_document());
  const std::string expected =
      "<doctag><title>Quarterly Report</title>\n"
      "<section_header_level_1>Overview</section_header_level_1>\n"
      "<text>Plain body text.</text>\n"
      "<unordered_list><list_item>alpha</list_item>\n"
      "<list_item>beta</list_item>\n"
      "<list_item><ordered_list><list_item>one</list_item>\n"
      "<list_item>two</list_item>\n"
      "</ordered_list></list_item>\n"
      "</unordered_list>\n"
      "<otsl><ched>Fuel<ched>Rate|Unit<nl><fcel>Diesel<fcel>0.9<nl>"
      "<caption>Fuel table</caption></otsl>\n"
      "<code><_Python_>print('hi')</code>\n"
      "<formula>E = mc^2</formula>\n"
      "<formula></formula>\n"
      "<picture><caption>A chart</caption></picture>\n"
      "<checkbox_selected>Done</checkbox_selected>\n"
      "<key_value_region></key_value_region>\n"
      "</doctag>";
  require(doctags == expected,
          "doctags export differs from the docling-parity expectation:\n" + doctags);
}

void verify_doctags_otsl_spans_and_locations() {
  docv1::Document document = base_document("spans.pdf");
  auto& page = (*document.mutable_pages())[1];
  page.set_page_no(1);
  page.mutable_size()->set_width(200.0);
  page.mutable_size()->set_height(100.0);

  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "hello");
  add_prov_to_last_text(&document, 1, 20.0, 10.0, 100.0, 30.0);

  // A 2x3 grid via flat cells: "A" spans two rows, "B" spans two columns.
  // Continuations must emit <ucel> under the vertical span and <lcel> after
  // the horizontal one.
  auto* table = add_table(&document, "#/body");
  auto* data = table->mutable_data();
  data->set_num_rows(2);
  data->set_num_cols(3);
  const auto cell = [&data](const std::string& text, int row, int row_span, int col,
                            int col_span) {
    auto* added = data->add_table_cells();
    added->set_text(text);
    added->set_row_span(row_span);
    added->set_col_span(col_span);
    added->set_start_row_offset_idx(row);
    added->set_end_row_offset_idx(row + row_span);
    added->set_start_col_offset_idx(col);
    added->set_end_col_offset_idx(col + col_span);
  };
  cell("A", 0, 2, 0, 1);
  cell("B", 0, 1, 1, 2);
  cell("C", 1, 1, 1, 1);
  cell("D", 1, 1, 2, 1);

  // A 2x2 grid fully covered by one spanning cell: every continuation of a
  // two-dimensional span is <xcel>.
  auto* cross = add_table(&document, "#/body");
  auto* cross_data = cross->mutable_data();
  cross_data->set_num_rows(2);
  cross_data->set_num_cols(2);
  auto* wide = cross_data->add_table_cells();
  wide->set_text("E");
  wide->set_row_span(2);
  wide->set_col_span(2);
  wide->set_end_row_offset_idx(2);
  wide->set_end_col_offset_idx(2);

  const std::string doctags = grparse::render_doctags(document);
  const std::string expected =
      "<doctag><text><loc_50><loc_50><loc_250><loc_150>hello</text>\n"
      "<otsl><fcel>A<fcel>B<lcel><nl><ucel><fcel>C<fcel>D<nl></otsl>\n"
      "<otsl><fcel>E<xcel><nl><xcel><xcel><nl></otsl>\n"
      "</doctag>";
  require(doctags == expected,
          "doctags OTSL spans or locations differ:\n" + doctags);
}

void verify_doclang_renders_grpc_xml_vocabulary() {
  const std::string doclang = grparse::render_doclang(rich_document());
  const std::string expected =
      "<doclang xmlns=\"http://docling-project.org/ns/doclang/v1\">\n"
      "  <title>Quarterly Report</title>\n"
      "  <section-header level=\"1\">Overview</section-header>\n"
      "  <paragraph>Plain body text.</paragraph>\n"
      "  <list ordered=\"false\">\n"
      "    <list-item>alpha</list-item>\n"
      "    <list-item>beta</list-item>\n"
      "    <list ordered=\"true\">\n"
      "      <list-item ordinal=\"1\">one</list-item>\n"
      "      <list-item ordinal=\"2\">two</list-item>\n"
      "    </list>\n"
      "  </list>\n"
      "  <caption>Fuel table</caption>\n"
      "  <table>\n"
      "    <tr>\n"
      "      <th>Fuel</th>\n"
      "      <th>Rate|Unit</th>\n"
      "    </tr>\n"
      "    <tr>\n"
      "      <td>Diesel</td>\n"
      "      <td>0.9</td>\n"
      "    </tr>\n"
      "  </table>\n"
      "  <code language=\"python\">print('hi')</code>\n"
      "  <formula>E = mc^2</formula>\n"
      "  <formula></formula>\n"
      "  <caption>A chart</caption>\n"
      "  <picture uri=\"figure1.png\"/>\n"
      "  <picture/>\n"
      "  <paragraph>Done</paragraph>\n"
      "  <!-- key-value item omitted -->\n"
      "</doclang>";
  require(doclang == expected,
          "doclang export differs from the round-trip vocabulary:\n" + doclang);
  // The structural bar: the root element grpc-xml's sniffer keys on.
  require(doclang.starts_with("<doclang xmlns=\"http://docling-project.org/ns/doclang/v1\">"),
          "doclang export must declare the NS_DOCLANG root");
}

void verify_doclang_escapes_xml_content() {
  docv1::Document document = base_document("escape.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "A & B < C > D");
  auto* figure = add_picture(&document, "#/body", "figs/a&b.png");
  figure->add_captions()->set_ref(add_caption(&document, figure->self_ref(), "Q \"quoted\""));
  const std::string doclang = grparse::render_doclang(document);
  require(doclang.contains("<paragraph>A &amp; B &lt; C &gt; D</paragraph>"),
          "doclang text content must be XML-escaped:\n" + doclang);
  require(doclang.contains("<picture uri=\"figs/a&amp;b.png\"/>"),
          "doclang attributes must be XML-escaped:\n" + doclang);
  require(doclang.contains("<caption>Q \"quoted\"</caption>"),
          "quotes in element text need no escaping:\n" + doclang);
}

// Appends one track-timed text item; extra_collector_source prepends a
// CollectorSource entry to prove the renderer scans past attribution.
void add_timed_text(docv1::Document* document, const std::string& text,
                    double start, double end, const std::string& voice,
                    const std::string& identifier, bool extra_collector_source = false) {
  add_text(document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, text);
  auto* base = document->mutable_texts(document->texts_size() - 1)
                   ->mutable_text()
                   ->mutable_base();
  if (extra_collector_source) {
    base->add_source()->mutable_collector()->set_collector("asr");
  }
  auto* track = base->add_source()->mutable_track();
  track->set_start_time(start);
  track->set_end_time(end);
  if (!voice.empty()) track->set_voice(voice);
  if (!identifier.empty()) track->set_identifier(identifier);
}

void verify_vtt_renders_timed_cues() {
  docv1::Document document = base_document("meeting.wav");
  add_text(&document, "#/body", docv1::BaseTextItem::kTitle,
           docv1::DOC_ITEM_LABEL_TITLE, "Meeting");
  add_timed_text(&document, "Hello there", 1.5, 3.25, "Alice", "", true);
  // An untimed text item produces no cue.
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "no timing");
  add_timed_text(&document, "line one", 10.0, 12.0, "", "cue-3");
  add_timed_text(&document, "line two", 10.0, 12.0, "", "cue-3");
  // Past one hour, with a fractional end that must round into the seconds
  // field instead of printing a four-digit millisecond count.
  add_timed_text(&document, "Later cue", 3725.5, 3727.0, "", "");
  add_timed_text(&document, "Rounded", 7199.999, 7199.9996, "", "");

  const std::string vtt = grparse::render_vtt(document);
  const std::string expected =
      "WEBVTT Meeting\n"
      "\n"
      "00:00:01.500 --> 00:00:03.250\n"
      "<v Alice>Hello there</v>\n"
      "\n"
      "cue-3\n"
      "00:00:10.000 --> 00:00:12.000\n"
      "line one\n"
      "line two\n"
      "\n"
      "01:02:05.500 --> 01:02:07.000\n"
      "Later cue\n"
      "\n"
      "01:59:59.999 --> 02:00:00.000\n"
      "Rounded";
  require(vtt == expected, "vtt export differs:\n" + vtt);

  require(grparse::render_vtt(base_document("silence.wav")) == "WEBVTT",
          "a document with no timed items renders the bare WEBVTT header");
}

void verify_split_page_assigns_by_provenance() {
  docv1::Document document = base_document("split.pdf");
  auto& first_page = (*document.mutable_pages())[1];
  first_page.set_page_no(1);
  first_page.mutable_size()->set_width(100.0);
  first_page.mutable_size()->set_height(100.0);
  first_page.mutable_image()->set_uri("page1.png");
  auto& second_page = (*document.mutable_pages())[2];
  second_page.set_page_no(2);
  second_page.mutable_size()->set_width(100.0);
  second_page.mutable_size()->set_height(100.0);

  // A leading un-provenanced item joins the first provenanced page; a
  // trailing one stays with the page in effect.
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "T0");
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "A");
  add_prov_to_last_text(&document, 1, 0, 0, 10, 10);
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "B");
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "C");
  add_prov_to_last_text(&document, 2, 0, 0, 10, 10);

  const std::string html = grparse::render_html_split_page(document);
  const std::string expected =
      "<!DOCTYPE html>\n<html>\n<head><meta charset=\"UTF-8\"/>"
      "<title>split.pdf</title></head>\n<body>\n"
      "<table>\n<tbody>\n"
      "<tr>\n<td>\n"
      "<figure><img src=\"page1.png\"></figure>\n"
      "</td>\n<td>\n<div class='page'>\n"
      "<p>T0</p>\n<p>A</p>\n<p>B</p>\n"
      "</div>\n</td>\n</tr>\n"
      "<tr>\n<td>\n"
      "<figure>no page-image found</figure>\n"
      "</td>\n<td>\n<div class='page'>\n"
      "<p>C</p>\n"
      "</div>\n</td>\n</tr>\n"
      "</tbody>\n</table>\n</body>\n</html>";
  require(html == expected, "split-page assignment differs:\n" + html);
}

void verify_split_page_without_provenance_is_one_page() {
  docv1::Document document = base_document("flat.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kTitle,
           docv1::DOC_ITEM_LABEL_TITLE, "Only");
  add_text(&document, "#/body", docv1::BaseTextItem::kText,
           docv1::DOC_ITEM_LABEL_TEXT, "content");
  const std::string html = grparse::render_html_split_page(document);
  require(html.contains("<div class='page'>") &&
              html.find("<div class='page'>") == html.rfind("<div class='page'>"),
          "a document with no provenance renders exactly one page:\n" + html);
  require(html.contains("<figure>no page-image found</figure>"),
          "a page without an image renders docling's placeholder");
  require(html.contains("<h1>Only</h1>") &&
              html.contains("<p>content</p>"),
          "the lone page holds the whole body");
}

void verify_yaml_matches_json_structure() {
  const std::string yaml = grparse::render_yaml(rich_document());
  require_contains(yaml, "name: report.pdf", "yaml keeps the document name");
  require_contains(yaml, "texts:", "yaml keeps the text arena");
  require_contains(yaml, "#/body", "yaml keeps reference strings");
  require_contains(yaml, "self_ref:", "yaml preserves proto field names");
  require(!yaml.contains("selfRef"),
          "yaml must not use camelCase field names");
  require(!yaml.starts_with('{'), "yaml renders block style, not flow JSON");
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
  require(grparse::render_doctags(document) == "<doctag>\n</doctag>",
          "an empty body renders the bare doctag wrapper");
  require(grparse::render_doclang(document) ==
              "<doclang xmlns=\"http://docling-project.org/ns/doclang/v1\">\n</doclang>",
          "an empty body renders the bare doclang root");
  require(grparse::render_vtt(document) == "WEBVTT",
          "an empty body renders the bare WEBVTT header");
  require(!grparse::render_yaml(document).empty(), "an empty body still renders yaml");
}

void verify_json_preserves_field_names_and_round_trips() {
  const docv1::Document document = rich_document();
  const std::string json = grparse::render_json(document);
  require_contains(json, "\"self_ref\"", "json keeps proto field names");
  require(!json.contains("\"selfRef\""),
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
  return grparse_test::run_test_main("document-render-test", {
      verify_markdown_renders_every_item_type,
      verify_markdown_reconstructs_grid_from_flat_cells,
      verify_markdown_multiline_cells_stay_single_line,
      verify_markdown_escaping_marker_and_formatting_rules,
      verify_markdown_custom_meta_field_order,
      verify_html_renders_structure_and_escapes,
      verify_non_body_layers_are_excluded,
      verify_captions_render_once,
      verify_picture_descriptions_surface_in_exports,
      verify_doctags_renders_every_item_type,
      verify_doctags_otsl_spans_and_locations,
      verify_doclang_renders_grpc_xml_vocabulary,
      verify_doclang_escapes_xml_content,
      verify_vtt_renders_timed_cues,
      verify_split_page_assigns_by_provenance,
      verify_split_page_without_provenance_is_one_page,
      verify_yaml_matches_json_structure,
      verify_empty_document_renders,
      verify_json_preserves_field_names_and_round_trips,
  });
}
