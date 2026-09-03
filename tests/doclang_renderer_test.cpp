// The DocLang XML export, unit by unit: the root element, the vocabulary each
// item maps to, list ordinals and nesting depth, table spans and gaps, the
// picture element with and without a description, and the comments the
// unserved arenas leave.  Whole-document parity for the same renderer lives
// in document_render_test.cpp; these cases pin the pieces that file does not
// reach.

#include <cstddef>
#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;

using grparse::render_doclang;
using grparse_test::add_cell;
using grparse_test::add_code;
using grparse_test::add_group;
using grparse_test::add_heading;
using grparse_test::add_owned_group;
using grparse_test::add_owned_text;
using grparse_test::add_paragraph;
using grparse_test::add_picture;
using grparse_test::add_table;
using grparse_test::add_text;
using grparse_test::base_document;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

const std::string kRoot = "<doclang xmlns=\"http://docling-project.org/ns/doclang/v1\">\n";

// Everything between the root element's tags, so a case states the elements
// it expects and nothing else.
std::string body_of(const docv1::Document& document) {
  const std::string doclang = render_doclang(document);
  require(doclang.starts_with(kRoot), "the export must open the DocLang root:\n" + doclang);
  require(doclang.ends_with("</doclang>"), "the export must close its root:\n" + doclang);
  const std::size_t tail = std::string("</doclang>").size();
  return doclang.substr(kRoot.size(), doclang.size() - kRoot.size() - tail);
}

void verify_an_empty_document_is_the_bare_root() {
  require_equal(render_doclang(base_document("empty.pdf")), kRoot + "</doclang>",
                "an empty document renders the root element and nothing else");
}

void verify_each_text_variant_takes_its_own_element() {
  docv1::Document document = base_document("vocabulary.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kTitle, docv1::DOC_ITEM_LABEL_TITLE, "Doc");
  add_text(&document, "#/body", docv1::BaseTextItem::kSectionHeader,
           docv1::DOC_ITEM_LABEL_SECTION_HEADER, "unset level", 0);
  add_text(&document, "#/body", docv1::BaseTextItem::kSectionHeader,
           docv1::DOC_ITEM_LABEL_SECTION_HEADER, "deep", 4);
  add_paragraph(&document, "#/body", "prose");
  add_text(&document, "#/body", docv1::BaseTextItem::kFormula, docv1::DOC_ITEM_LABEL_FORMULA,
           "E = mc^2");
  add_text(&document, "#/body", docv1::BaseTextItem::kText, docv1::DOC_ITEM_LABEL_FOOTNOTE,
           "a footnote");
  add_text(&document, "#/body", docv1::BaseTextItem::kText, docv1::DOC_ITEM_LABEL_REFERENCE,
           "a reference");
  add_text(&document, "#/body", docv1::BaseTextItem::kText, docv1::DOC_ITEM_LABEL_CAPTION,
           "a loose caption");
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "a stray item");
  add_paragraph(&document, "#/body", "");

  require_equal(body_of(document),
                "  <title>Doc</title>\n"
                "  <section-header level=\"1\">unset level</section-header>\n"
                "  <section-header level=\"4\">deep</section-header>\n"
                "  <paragraph>prose</paragraph>\n"
                "  <formula>E = mc^2</formula>\n"
                "  <footnote>a footnote</footnote>\n"
                "  <reference>a reference</reference>\n"
                "  <caption>a loose caption</caption>\n"
                "  <list-item>a stray item</list-item>\n",
                "every variant and label maps to the vocabulary grpc-xml reads back");
}

void verify_code_carries_its_language_only_when_it_has_one() {
  docv1::Document document = base_document("code.md");
  add_code(&document, "#/body", "int main() {}", docv1::CODE_LANGUAGE_LABEL_C_PLUS_PLUS);
  add_code(&document, "#/body", "plain", docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED);
  add_code(&document, "#/body", "x", docv1::CODE_LANGUAGE_LABEL_UNKNOWN);
  require_equal(body_of(document),
                "  <code language=\"cpp\">int main() {}</code>\n"
                "  <code>plain</code>\n"
                "  <code>x</code>\n",
                "a known language becomes an attribute and an unknown one is left off");
}

void verify_lists_number_only_when_they_are_ordered() {
  docv1::Document document = base_document("lists.md");
  const std::string plain = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_text(&document, plain, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "alpha");
  const std::string ordered = add_group(&document, "#/body", docv1::GROUP_LABEL_ORDERED_LIST);
  add_text(&document, ordered, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "one", 0, true);
  add_text(&document, ordered, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "two", 0, true);

  require_equal(body_of(document),
                "  <list ordered=\"false\">\n"
                "    <list-item>alpha</list-item>\n"
                "  </list>\n"
                "  <list ordered=\"true\">\n"
                "    <list-item ordinal=\"1\">one</list-item>\n"
                "    <list-item ordinal=\"2\">two</list-item>\n"
                "  </list>\n",
                "an ordered list numbers its items and an unordered one does not");
}

void verify_a_nested_list_indents_one_level_further() {
  docv1::Document document = base_document("lists.md");
  const std::string outer = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_text(&document, outer, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "alpha");
  const std::string inner = add_group(&document, outer, docv1::GROUP_LABEL_LIST);
  add_text(&document, inner, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "deep");

  require_equal(body_of(document),
                "  <list ordered=\"false\">\n"
                "    <list-item>alpha</list-item>\n"
                "    <list ordered=\"false\">\n"
                "      <list-item>deep</list-item>\n"
                "    </list>\n"
                "  </list>\n",
                "a nested list is a sibling element one indent deeper");
}

void verify_an_empty_list_group_still_writes_its_element() {
  docv1::Document document = base_document("lists.md");
  add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  require_equal(body_of(document), "  <list ordered=\"false\">\n  </list>\n",
                "a list group with no items still opens and closes its element");
}

void verify_tables_carry_spans_gaps_and_header_cells() {
  docv1::Document document = base_document("spans.pdf");
  auto* table = add_table(&document, "#/body");
  table->add_captions()->set_ref(
      add_owned_text(&document, table->self_ref(), docv1::DOC_ITEM_LABEL_CAPTION, "Fuel"));
  auto* data = table->mutable_data();
  data->set_num_rows(2);
  data->set_num_cols(3);
  add_cell(data, nullptr, "A", true, 0, 0, 2, 1);
  add_cell(data, nullptr, "B", false, 0, 1, 1, 2);
  add_cell(data, nullptr, "C", false, 1, 1);

  require_equal(body_of(document),
                "  <caption>Fuel</caption>\n"
                "  <table>\n"
                "    <tr>\n"
                "      <th rowspan=\"2\">A</th>\n"
                "      <td colspan=\"2\">B</td>\n"
                "    </tr>\n"
                "    <tr>\n"
                "      <td>C</td>\n"
                "      <td></td>\n"
                "    </tr>\n"
                "  </table>\n",
                "spans become attributes, a gap becomes an empty data cell, and the caption "
                "stands before the table");
}

void verify_a_table_with_no_grid_writes_only_its_caption() {
  docv1::Document document = base_document("caption-only.pdf");
  auto* table = add_table(&document, "#/body");
  table->add_captions()->set_ref(
      add_owned_text(&document, table->self_ref(), docv1::DOC_ITEM_LABEL_CAPTION, "Alone"));
  require_equal(body_of(document), "  <caption>Alone</caption>\n",
                "a table with no cells still shows the caption it carries");
}

void verify_a_rich_cell_renders_its_blocks_inside_the_cell() {
  docv1::Document document = base_document("rich-cells.pdf");
  auto* table = add_table(&document, "#/body");
  auto* data = table->mutable_data();
  data->set_num_rows(2);
  data->set_num_cols(2);
  add_cell(data, nullptr, "plain", false, 0, 0);

  const std::string list_blocks =
      add_owned_group(&document, table->self_ref(), docv1::GROUP_LABEL_UNSPECIFIED);
  const std::string list = add_group(&document, list_blocks, docv1::GROUP_LABEL_LIST);
  add_text(&document, list, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "Pond");
  add_text(&document, list, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "Marsh");
  add_cell(data, nullptr, "", false, 0, 1)->mutable_ref()->set_ref(list_blocks);

  const std::string heading_blocks =
      add_owned_group(&document, table->self_ref(), docv1::GROUP_LABEL_UNSPECIFIED);
  add_heading(&document, heading_blocks, "Overview", 1);
  add_cell(data, nullptr, "", false, 1, 0)->mutable_ref()->set_ref(heading_blocks);

  const std::string table_blocks =
      add_owned_group(&document, table->self_ref(), docv1::GROUP_LABEL_UNSPECIFIED);
  auto* nested = add_table(&document, table_blocks);
  nested->mutable_data()->set_num_rows(1);
  nested->mutable_data()->set_num_cols(1);
  add_cell(nested->mutable_data(), nullptr, "Sound", false, 0, 0);
  add_cell(data, nullptr, "", false, 1, 1)->mutable_ref()->set_ref(table_blocks);

  require_equal(body_of(document),
                "  <table>\n"
                "    <tr>\n"
                "      <td>plain</td>\n"
                "      <td>\n"
                "        <list ordered=\"false\">\n"
                "          <list-item>Pond</list-item>\n"
                "          <list-item>Marsh</list-item>\n"
                "        </list>\n"
                "      </td>\n"
                "    </tr>\n"
                "    <tr>\n"
                "      <td>\n"
                "        <section-header level=\"1\">Overview</section-header>\n"
                "      </td>\n"
                "      <td>\n"
                "        <table>\n"
                "          <tr>\n"
                "            <td>Sound</td>\n"
                "          </tr>\n"
                "        </table>\n"
                "      </td>\n"
                "    </tr>\n"
                "  </table>\n",
                "a rich cell opens its element and renders the group's blocks one indent "
                "deeper: a list, a heading, and a nested table");
}

void verify_a_linked_caption_takes_the_block_form() {
  docv1::Document document = base_document("linked-figure.pdf");
  auto* figure = add_picture(&document, "#/body", "figs/a.png");
  figure->add_captions()->set_ref(add_owned_text(&document, figure->self_ref(),
                                                 docv1::DOC_ITEM_LABEL_CAPTION, "A chart"));
  document.mutable_texts(document.texts_size() - 1)
      ->mutable_text()
      ->mutable_base()
      ->set_hyperlink("https://example.com/chart");

  require_equal(body_of(document),
                "  <caption>\n"
                "    <href uri=\"https://example.com/chart\"/>\n"
                "    A chart\n"
                "  </caption>\n"
                "  <picture uri=\"figs/a.png\"/>\n",
                "a caption carrying a hyperlink renders block-form with an href head");

  docv1::Document plain = base_document("plain-figure.pdf");
  auto* bare = add_picture(&plain, "#/body", "figs/a.png");
  bare->add_captions()->set_ref(add_owned_text(&plain, bare->self_ref(),
                                               docv1::DOC_ITEM_LABEL_CAPTION, "A chart"));
  require_equal(body_of(plain),
                "  <caption>A chart</caption>\n"
                "  <picture uri=\"figs/a.png\"/>\n",
                "a caption without a hyperlink keeps the plain inline form");
}

void verify_a_linked_table_caption_normalizes_its_href() {
  docv1::Document document = base_document("linked-table.pdf");
  auto* table = add_table(&document, "#/body");
  table->add_captions()->set_ref(
      add_owned_text(&document, table->self_ref(), docv1::DOC_ITEM_LABEL_CAPTION, "Fuel"));
  document.mutable_texts(document.texts_size() - 1)
      ->mutable_text()
      ->mutable_base()
      ->set_hyperlink("https://EXAMPLE.com");

  require_equal(body_of(document),
                "  <caption>\n"
                "    <href uri=\"https://example.com/\"/>\n"
                "    Fuel\n"
                "  </caption>\n",
                "a table caption link renders the same block form, host lowercased and "
                "the empty path made explicit");
}

void verify_a_picture_element_states_its_uri_and_description() {
  docv1::Document bare = base_document("figure.pdf");
  add_picture(&bare, "#/body", "");
  require_equal(body_of(bare), "  <picture/>\n",
                "a picture with no image is an empty element");

  docv1::Document located = base_document("figure.pdf");
  add_picture(&located, "#/body", "figs/a.png");
  require_equal(body_of(located), "  <picture uri=\"figs/a.png\"/>\n",
                "a picture with an image states its uri");

  docv1::Document described = base_document("figure.pdf");
  add_picture(&described, "#/body", "figs/a.png")
      ->mutable_meta()
      ->mutable_description()
      ->set_text("  a bar chart  ");
  require_equal(body_of(described),
                "  <picture uri=\"figs/a.png\">\n"
                "    <description>a bar chart</description>\n"
                "  </picture>\n",
                "a description trims and nests inside the picture element");
}

void verify_the_unserved_arenas_leave_a_comment() {
  docv1::Document document = base_document("kv.pdf");
  document.mutable_body()->add_children()->set_ref("#/key_value_items/0");
  document.mutable_body()->add_children()->set_ref("#/form_items/0");
  require_equal(body_of(document),
                "  <!-- key-value item omitted -->\n  <!-- form item omitted -->\n",
                "the arenas with no DocLang element name their omission");
}

void verify_content_and_attributes_are_xml_escaped() {
  docv1::Document document = base_document("escape.pdf");
  add_paragraph(&document, "#/body", "a < b & c > d");
  add_picture(&document, "#/body", "figs/a&b\"c\".png");
  require_equal(body_of(document),
                "  <paragraph>a &lt; b &amp; c &gt; d</paragraph>\n"
                "  <picture uri=\"figs/a&amp;b&quot;c&quot;.png\"/>\n",
                "text escapes the markup specials and an attribute escapes the quote too");
}

void verify_a_transparent_group_adds_no_element_and_no_indent() {
  docv1::Document document = base_document("chapter.epub");
  const std::string chapter = add_group(&document, "#/body", docv1::GROUP_LABEL_CHAPTER);
  add_paragraph(&document, chapter, "inside");
  require_equal(body_of(document), "  <paragraph>inside</paragraph>\n",
                "a chapter group is transparent, so its child keeps the body's indent");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("doclang-renderer-test", "ok", {
      verify_an_empty_document_is_the_bare_root,
      verify_each_text_variant_takes_its_own_element,
      verify_code_carries_its_language_only_when_it_has_one,
      verify_lists_number_only_when_they_are_ordered,
      verify_a_nested_list_indents_one_level_further,
      verify_an_empty_list_group_still_writes_its_element,
      verify_tables_carry_spans_gaps_and_header_cells,
      verify_a_rich_cell_renders_its_blocks_inside_the_cell,
      verify_a_table_with_no_grid_writes_only_its_caption,
      verify_a_linked_caption_takes_the_block_form,
      verify_a_linked_table_caption_normalizes_its_href,
      verify_a_picture_element_states_its_uri_and_description,
      verify_the_unserved_arenas_leave_a_comment,
      verify_content_and_attributes_are_xml_escaped,
      verify_a_transparent_group_adds_no_element_and_no_indent,
  });
}
