// The structural HTML export, unit by unit: the skeleton, heading ranks,
// paragraphs and their soft line breaks, lists (nested, enumerated, stray),
// tables with row and column spans, figures with captions and descriptions,
// code, formulas, and the split-page layout's page rows.  Whole-document
// parity for the same renderer lives in document_render_test.cpp; these
// cases pin the pieces that file does not reach.

#include <cstddef>
#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;

using grparse::render_html;
using grparse::render_html_split_page;
using grparse_test::add_cell;
using grparse_test::add_code;
using grparse_test::add_group;
using grparse_test::add_owned_text;
using grparse_test::add_page;
using grparse_test::add_paragraph;
using grparse_test::add_picture;
using grparse_test::add_prov;
using grparse_test::add_table;
using grparse_test::add_text;
using grparse_test::base_document;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

const std::string kSkeletonTail = "</body>\n</html>";

std::string skeleton(const std::string& title) {
  return "<!DOCTYPE html>\n<html>\n<head><meta charset=\"UTF-8\"/><title>" + title +
         "</title></head>\n<body>\n";
}

// The body of a rendered page: everything between the skeleton's <body> and
// its close, so a case can state the elements it expects and nothing else.
std::string body_of(const std::string& html, const std::string& title) {
  const std::string open = skeleton(title);
  require(html.starts_with(open), "the export must open the skeleton:\n" + html);
  require(html.ends_with(kSkeletonTail), "the export must close the skeleton:\n" + html);
  return html.substr(open.size(), html.size() - open.size() - kSkeletonTail.size());
}

void require_body(const docv1::Document& document, const std::string& expected,
                  const std::string& what) {
  require_equal(body_of(render_html(document), document.name()), expected, what);
}

void verify_an_empty_document_renders_the_bare_skeleton() {
  const docv1::Document document = base_document("empty.pdf");
  require_equal(render_html(document), skeleton("empty.pdf") + kSkeletonTail,
                "an empty document renders the skeleton and nothing else");
}

void verify_the_document_name_is_the_escaped_title() {
  const docv1::Document document = base_document("a & b <c>.pdf");
  require(render_html(document).contains("<title>a &amp; b &lt;c&gt;.pdf</title>"),
          "the document name is HTML-escaped into the title element");
}

void verify_heading_ranks_clamp_between_h2_and_h6() {
  docv1::Document document = base_document("headings.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kTitle, docv1::DOC_ITEM_LABEL_TITLE, "Doc");
  for (const int level : {0, 1, 2, 5, 9}) {
    add_text(&document, "#/body", docv1::BaseTextItem::kSectionHeader,
             docv1::DOC_ITEM_LABEL_SECTION_HEADER, "L" + std::to_string(level), level);
  }
  require_body(document,
               "<h1>Doc</h1>\n<h2>L0</h2>\n<h2>L1</h2>\n<h3>L2</h3>\n<h6>L5</h6>\n<h6>L9</h6>\n",
               "the title takes h1 and section headers start at h2 and clamp at h6");
}

void verify_paragraph_newlines_become_soft_breaks() {
  docv1::Document document = base_document("breaks.pdf");
  add_paragraph(&document, "#/body", "first\r\nsecond\nthird");
  add_paragraph(&document, "#/body", "");
  require_body(document, "<p>first<br>second<br>third</p>\n",
               "newlines become <br>, carriage returns are dropped, empty text renders nothing");
}

void verify_nested_lists_nest_their_markup() {
  docv1::Document document = base_document("lists.pdf");
  const std::string outer = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_text(&document, outer, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "alpha");
  const std::string inner = add_group(&document, outer, docv1::GROUP_LABEL_ORDERED_LIST);
  add_text(&document, inner, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "one", 0, true);
  const std::string deepest = add_group(&document, inner, docv1::GROUP_LABEL_LIST);
  add_text(&document, deepest, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "deep");

  require_body(document,
               "<ul><li>alpha</li><li><ol><li>one</li><li><ul><li>deep</li></ul></li></ol></li>"
               "</ul>\n",
               "each nested list rides inside a list item of its parent");
}

void verify_an_enumerated_item_makes_an_unlabelled_group_ordered() {
  docv1::Document document = base_document("lists.pdf");
  const std::string list = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_text(&document, list, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "one", 0, true);
  add_text(&document, list, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "two", 0, true);
  require_body(document, "<ol><li>one</li><li>two</li></ol>\n",
               "the first list item's numbering decides the list element");
}

void verify_a_stray_list_item_reads_as_a_one_item_list() {
  docv1::Document document = base_document("lists.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "orphan");
  require_body(document, "<ul><li>orphan</li></ul>\n",
               "a list item outside a list group still renders as a list");
}

void verify_tables_carry_their_spans_and_gaps() {
  docv1::Document document = base_document("spans.pdf");
  auto* table = add_table(&document, "#/body");
  auto* data = table->mutable_data();
  data->set_num_rows(2);
  data->set_num_cols(3);
  docv1::TableRow row;
  add_cell(data, &row, "A", true, 0, 0, 2, 1);
  add_cell(data, &row, "B", false, 0, 1, 1, 2);
  add_cell(data, &row, "C", false, 1, 1);
  add_cell(data, &row, "D", false, 1, 2);

  require_body(document,
               "<table><tbody>"
               "<tr><th rowspan=\"2\">A</th><td colspan=\"2\">B</td></tr>"
               "<tr><td>C</td><td>D</td></tr>"
               "</tbody></table>\n",
               "a spanned cell is written once, at the first position it covers");
}

void verify_a_position_no_cell_reaches_is_an_empty_data_cell() {
  docv1::Document document = base_document("gaps.pdf");
  auto* table = add_table(&document, "#/body");
  auto* data = table->mutable_data();
  data->set_num_rows(1);
  data->set_num_cols(2);
  docv1::TableRow row;
  add_cell(data, &row, "X", false, 0, 0);

  require_body(document, "<table><tbody><tr><td>X</td><td></td></tr></tbody></table>\n",
               "a grid position no cell covers renders as an empty data cell");
}

void verify_row_headers_and_row_sections_are_header_cells() {
  docv1::Document document = base_document("headers.pdf");
  auto* table = add_table(&document, "#/body");
  auto* data = table->mutable_data();
  data->set_num_rows(1);
  data->set_num_cols(2);
  docv1::TableRow row;
  add_cell(data, &row, "left", false, 0, 0)->set_row_header(true);
  add_cell(data, &row, "section", false, 0, 1)->set_row_section(true);

  require_body(document, "<table><tbody><tr><th>left</th><th>section</th></tr></tbody></table>\n",
               "a row header and a row section are both header cells");
}

void verify_a_table_with_neither_cells_nor_caption_renders_nothing() {
  docv1::Document document = base_document("empty-table.pdf");
  add_table(&document, "#/body");
  require_body(document, "", "a table with no grid and no caption contributes no element");
}

void verify_a_captioned_picture_with_no_image_keeps_the_placeholder() {
  docv1::Document document = base_document("figure.pdf");
  auto* picture = add_picture(&document, "#/body", "");
  picture->add_captions()->set_ref(
      add_owned_text(&document, picture->self_ref(), docv1::DOC_ITEM_LABEL_CAPTION, "Figure 1"));
  require_body(document,
               "<figure><!-- image --><figcaption>Figure 1</figcaption></figure>\n",
               "a picture with no image keeps the placeholder and still shows its caption");
}

void verify_two_captions_each_get_their_own_figcaption() {
  docv1::Document document = base_document("figure.pdf");
  auto* picture = add_picture(&document, "#/body", "a.png");
  picture->add_captions()->set_ref(
      add_owned_text(&document, picture->self_ref(), docv1::DOC_ITEM_LABEL_CAPTION, "First"));
  picture->add_captions()->set_ref(
      add_owned_text(&document, picture->self_ref(), docv1::DOC_ITEM_LABEL_CAPTION, "Second"));
  require_body(document,
               "<figure><img src=\"a.png\" alt=\"First\"/><figcaption>First</figcaption>"
               "<figcaption>Second</figcaption></figure>\n",
               "the first caption is the alt text and every caption gets an element");
}

void verify_an_undecoded_formula_says_so() {
  docv1::Document document = base_document("math.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kFormula, docv1::DOC_ITEM_LABEL_FORMULA, "");
  add_text(&document, "#/body", docv1::BaseTextItem::kFormula, docv1::DOC_ITEM_LABEL_FORMULA,
           "a < b");
  require_body(document,
               "<div class=\"formula-not-decoded\">Formula not decoded</div>\n"
               "<div class=\"formula\">a &lt; b</div>\n",
               "an empty formula renders the placeholder and a decoded one is escaped");
}

void verify_code_rides_a_preformatted_block() {
  docv1::Document document = base_document("code.md");
  add_code(&document, "#/body", "if (a < b) {}", docv1::CODE_LANGUAGE_LABEL_C_PLUS_PLUS);
  require_body(document, "<pre><code>if (a &lt; b) {}</code></pre>\n",
               "code is escaped and carries no language attribute");
}

void verify_an_inline_group_joins_its_children_into_one_paragraph() {
  docv1::Document document = base_document("inline.docx");
  const std::string inline_group = add_group(&document, "#/body", docv1::GROUP_LABEL_INLINE);
  add_paragraph(&document, inline_group, "one");
  add_paragraph(&document, inline_group, "two");
  require_body(document, "<p>one two</p>\n", "an inline group folds into a single paragraph");
}

void verify_unserved_arenas_leave_a_comment() {
  docv1::Document document = base_document("kv.pdf");
  document.mutable_body()->add_children()->set_ref("#/key_value_items/0");
  document.mutable_body()->add_children()->set_ref("#/form_items/0");
  require_body(document, "<!-- missing-key-value-item -->\n<!-- missing-form-item -->\n",
               "the key-value and form arenas render as the placeholders the model emits");
}

void verify_a_group_with_no_list_label_renders_only_its_children() {
  docv1::Document document = base_document("chapter.epub");
  const std::string chapter = add_group(&document, "#/body", docv1::GROUP_LABEL_CHAPTER);
  add_paragraph(&document, chapter, "inside a chapter");
  require_body(document, "<p>inside a chapter</p>\n",
               "a chapter group adds no markup of its own");
}

void verify_split_page_groups_elements_by_page() {
  docv1::Document document = base_document("two-pages.pdf");
  add_page(&document, 1, 100, 200);
  add_page(&document, 2, 100, 200)->mutable_image()->set_uri("p2.png");
  add_paragraph(&document, "#/body", "on one");
  add_prov(document.mutable_texts(0)->mutable_text()->mutable_base()->mutable_prov(), 1, 0, 0, 10,
           10);
  add_paragraph(&document, "#/body", "on two");
  add_prov(document.mutable_texts(1)->mutable_text()->mutable_base()->mutable_prov(), 2, 0, 0, 10,
           10);

  const std::string html = render_html_split_page(document);
  require(html.contains("<figure>no page-image found</figure>"),
          "a page with no image gets the model's placeholder figure:\n" + html);
  require(html.contains("<figure><img src=\"p2.png\"></figure>"),
          "a page with an image gets it:\n" + html);
  const std::size_t first = html.find("on one");
  const std::size_t second = html.find("on two");
  require(first != std::string::npos && second != std::string::npos && first < second,
          "each element lands in its own page row, in page order:\n" + html);
}

void verify_split_page_without_provenance_is_one_page() {
  docv1::Document document = base_document("one-page.pdf");
  add_paragraph(&document, "#/body", "no provenance");
  const std::string html = render_html_split_page(document);
  require_equal(html.find("<tr>"), html.rfind("<tr>"),
                "a document with no page provenance renders exactly one page row");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("html-renderer-test", "ok", {
      verify_an_empty_document_renders_the_bare_skeleton,
      verify_the_document_name_is_the_escaped_title,
      verify_heading_ranks_clamp_between_h2_and_h6,
      verify_paragraph_newlines_become_soft_breaks,
      verify_nested_lists_nest_their_markup,
      verify_an_enumerated_item_makes_an_unlabelled_group_ordered,
      verify_a_stray_list_item_reads_as_a_one_item_list,
      verify_tables_carry_their_spans_and_gaps,
      verify_a_position_no_cell_reaches_is_an_empty_data_cell,
      verify_row_headers_and_row_sections_are_header_cells,
      verify_a_table_with_neither_cells_nor_caption_renders_nothing,
      verify_a_captioned_picture_with_no_image_keeps_the_placeholder,
      verify_two_captions_each_get_their_own_figcaption,
      verify_an_undecoded_formula_says_so,
      verify_code_rides_a_preformatted_block,
      verify_an_inline_group_joins_its_children_into_one_paragraph,
      verify_unserved_arenas_leave_a_comment,
      verify_a_group_with_no_list_label_renders_only_its_children,
      verify_split_page_groups_elements_by_page,
      verify_split_page_without_provenance_is_one_page,
  });
}
