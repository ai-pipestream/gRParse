// The Markdown export, piece by piece, for the behavior added after the
// whole-document parity cases in document_render_test.cpp: footnotes on
// tables, pictures and code (docling-core #675), and hyperlink and image
// destinations (docling-core #920).

#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;

using grparse::render_markdown;
using grparse_test::add_cell;
using grparse_test::add_code;
using grparse_test::add_owned_group;
using grparse_test::add_owned_text;
using grparse_test::add_paragraph;
using grparse_test::add_picture;
using grparse_test::add_table;
using grparse_test::base_document;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

docv1::TableItem* one_cell_table(docv1::Document* document) {
  auto* table = add_table(document, "#/body");
  auto* data = table->mutable_data();
  data->set_num_rows(2);
  data->set_num_cols(1);
  docv1::TableRow header;
  add_cell(data, &header, "H", true, 0, 0);
  docv1::TableRow body;
  add_cell(data, &body, "X", false, 1, 0);
  return table;
}

void verify_table_footnotes_follow_the_body_one_paragraph_each() {
  docv1::Document document = base_document("footnotes.pdf");
  auto* table = one_cell_table(&document);
  table->add_footnotes()->set_ref(add_owned_text(&document, table->self_ref(),
                                                 docv1::DOC_ITEM_LABEL_FOOTNOTE, "n1"));
  table->add_footnotes()->set_ref(add_owned_text(&document, table->self_ref(),
                                                 docv1::DOC_ITEM_LABEL_FOOTNOTE, "n2"));
  require_equal(render_markdown(document), "| H   |\n|-----|\n| X   |\n\nn1\n\nn2",
                "table footnotes follow the table, one paragraph each");
}

void verify_a_claimed_footnote_is_not_repeated_from_the_body() {
  docv1::Document document = base_document("footnotes.pdf");
  auto* table = one_cell_table(&document);
  const std::string footnote = add_owned_text(&document, table->self_ref(),
                                              docv1::DOC_ITEM_LABEL_FOOTNOTE, "n1");
  table->add_footnotes()->set_ref(footnote);
  document.mutable_body()->add_children()->set_ref(footnote);
  require_equal(render_markdown(document), "| H   |\n|-----|\n| X   |\n\nn1",
                "a footnote the table claims is not rendered again from the body");
}

void verify_table_footnotes_follow_the_body_under_a_caption() {
  docv1::Document document = base_document("footnotes.pdf");
  auto* table = one_cell_table(&document);
  table->add_captions()->set_ref(add_owned_text(&document, table->self_ref(),
                                                docv1::DOC_ITEM_LABEL_CAPTION, "Table 1"));
  table->add_footnotes()->set_ref(add_owned_text(&document, table->self_ref(),
                                                 docv1::DOC_ITEM_LABEL_FOOTNOTE, "n1"));
  require_equal(render_markdown(document), "Table 1\n\n| H   |\n|-----|\n| X   |\n\nn1",
                "a table caption with no placement goes before and the footnote follows the body");
}

void verify_picture_footnotes_follow_the_image() {
  docv1::Document document = base_document("footnotes.pdf");
  auto* picture = add_picture(&document, "#/body", "");
  picture->add_footnotes()->set_ref(add_owned_text(&document, picture->self_ref(),
                                                   docv1::DOC_ITEM_LABEL_FOOTNOTE, "n1"));
  require_equal(render_markdown(document), "<!-- image -->\n\nn1",
                "picture footnotes follow the image placeholder");
}

void verify_code_footnotes_follow_the_code() {
  docv1::Document document = base_document("code.md");
  const std::string ref =
      add_code(&document, "#/body", "x = 1", docv1::CODE_LANGUAGE_LABEL_PYTHON);
  auto* code = document.mutable_texts(std::stoi(ref.substr(std::string("#/texts/").size())))
                   ->mutable_code();
  code->add_footnotes()->set_ref(
      add_owned_text(&document, ref, docv1::DOC_ITEM_LABEL_FOOTNOTE, "n1"));
  require_equal(render_markdown(document), "```\nx = 1\n```\n\nn1",
                "code footnotes follow the fenced block");
}

std::string linked_paragraph(const std::string& hyperlink) {
  docv1::Document document = base_document("links.md");
  add_paragraph(&document, "#/body", "c");
  document.mutable_texts(0)->mutable_text()->mutable_base()->set_hyperlink(hyperlink);
  return render_markdown(document);
}

void verify_hyperlinks_are_filtered_and_encoded() {
  require_equal(linked_paragraph("javascript:alert(1)"), std::string("c"),
                "an unsafe scheme keeps only the link text");
  require_equal(linked_paragraph("data:text/html,<script>1</script>"), std::string("c"),
                "a data: link keeps only the link text");
  require_equal(linked_paragraph("https://example.com/x"),
                std::string("[c](https://example.com/x)"), "an http link is kept");
  require_equal(linked_paragraph("sub/next.html"), std::string("[c](sub/next.html)"),
                "a relative reference is kept");
  require_equal(linked_paragraph("a(b)c d"), std::string("[c](a%28b%29c%20d)"),
                "destination characters that end a link are percent-encoded");
}

void verify_image_destinations_are_encoded() {
  docv1::Document document = base_document("image.md");
  add_picture(&document, "#/body", "data:image/png;base64,x\"><script>1</script>)<b>(");
  grparse::MarkdownOptions embedded;
  embedded.image_export_mode = grparse::MarkdownOptions::ImageExportMode::kEmbedded;
  require_equal(render_markdown(document, embedded),
                std::string("![](data:image/png;base64,x%22%3E%3Cscript%3E1%3C/script%3E%29%3Cb%3E%28)"),
                "an embedded image uri cannot end the image destination");
}

void verify_group_text_keeps_inline_boundary_space() {
  docv1::Document inline_doc = base_document("inline.md");
  const std::string runs =
      grparse_test::add_group(&inline_doc, "#/body", docv1::GROUP_LABEL_INLINE);
  add_paragraph(&inline_doc, runs, "hello ");
  add_paragraph(&inline_doc, runs, "world");
  require_equal(render_markdown(inline_doc), std::string("hello world"),
                "an inline group concatenates runs that carry their own spaces");

  const auto nested = [](docv1::GroupLabel label, const std::string& left,
                         const std::string& right) {
    docv1::Document document = base_document("nested.md");
    auto* outer = add_table(&document, "#/body");
    outer->mutable_data()->set_num_rows(1);
    outer->mutable_data()->set_num_cols(1);
    auto* inner = document.add_tables();
    const std::string inner_ref = "#/tables/" + std::to_string(document.tables_size() - 1);
    inner->set_self_ref(inner_ref);
    inner->mutable_parent()->set_ref(outer->self_ref());
    inner->set_label(docv1::DOC_ITEM_LABEL_TABLE);
    inner->set_content_layer(docv1::CONTENT_LAYER_BODY);
    inner->mutable_data()->set_num_rows(1);
    inner->mutable_data()->set_num_cols(1);
    const std::string group = add_owned_group(&document, inner_ref, label);
    add_paragraph(&document, group, left);
    add_paragraph(&document, group, right);
    add_cell(inner->mutable_data(), nullptr, "", false, 0, 0)->mutable_ref()->set_ref(group);
    add_cell(outer->mutable_data(), nullptr, "", false, 0, 0)->mutable_ref()->set_ref(inner_ref);
    return render_markdown(document);
  };

  const std::string inline_flat = nested(docv1::GROUP_LABEL_INLINE, "hello ", "world");
  require(inline_flat.find("hello world") != std::string::npos &&
              inline_flat.find("hello  world") == std::string::npos,
          "a nested inline group keeps one boundary space");
  const std::string block_flat = nested(docv1::GROUP_LABEL_UNSPECIFIED, "hello", "world");
  require(block_flat.find("hello world") != std::string::npos &&
              block_flat.find("helloworld") == std::string::npos,
          "a nested non-inline group joins its text with one space");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("markdown-renderer-test", "ok", {
      verify_table_footnotes_follow_the_body_one_paragraph_each,
      verify_a_claimed_footnote_is_not_repeated_from_the_body,
      verify_table_footnotes_follow_the_body_under_a_caption,
      verify_picture_footnotes_follow_the_image,
      verify_code_footnotes_follow_the_code,
      verify_hyperlinks_are_filtered_and_encoded,
      verify_image_destinations_are_encoded,
      verify_group_text_keeps_inline_boundary_space,
  });
}
