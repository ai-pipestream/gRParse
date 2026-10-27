// The Markdown export, piece by piece, for the behavior added after the
// whole-document parity cases in document_render_test.cpp: footnotes on
// tables, pictures and code (docling-core #675).

#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;

using grparse::render_markdown;
using grparse_test::add_cell;
using grparse_test::add_code;
using grparse_test::add_owned_text;
using grparse_test::add_picture;
using grparse_test::add_table;
using grparse_test::base_document;
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

}  // namespace

int main() {
  return grparse_test::run_test_main("markdown-renderer-test", "ok", {
      verify_table_footnotes_follow_the_body_one_paragraph_each,
      verify_a_claimed_footnote_is_not_repeated_from_the_body,
      verify_table_footnotes_follow_the_body_under_a_caption,
      verify_picture_footnotes_follow_the_image,
      verify_code_footnotes_follow_the_code,
  });
}
