// Two properties every export renderer owes its callers: rendering the same
// document twice produces the same bytes, and rendering never mutates the
// document it was handed.  Both are checked against one fixture that carries
// every arena and both unordered wire maps (pages, custom meta fields), since
// a hash-ordered container is the usual way a renderer stops being a pure
// function.

#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/util/message_differencer.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;

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

using Renderer = std::string (*)(const docv1::Document&);

// Every export the service serves, by the name its option carries.
const std::vector<std::pair<std::string, Renderer>>& renderers() {
  static const std::vector<std::pair<std::string, Renderer>> all{
      {"markdown", grparse::render_markdown},
      {"html", grparse::render_html},
      {"html_split_page", grparse::render_html_split_page},
      {"json", grparse::render_json},
      {"yaml", grparse::render_yaml},
      {"canonical_json", grparse::render_canonical_json},
      {"gdocs_json", grparse::render_gdocs_json},
      {"doctags", grparse::render_doctags},
      {"doclang", grparse::render_doclang},
      {"vtt", grparse::render_vtt},
      {"latex", grparse::render_latex},
  };
  return all;
}

// One document carrying every item kind a renderer branches on, plus the two
// unordered wire maps and a stray list item the load normalization rewrites.
docv1::Document every_item_document() {
  docv1::Document document = base_document("determinism.pdf");
  document.set_schema_name("pipestream_document_v1");
  document.set_version("1.0.0");
  // Pages inserted back to front: a proto map hands them over in hash order.
  for (const int page_no : {3, 1, 2}) add_page(&document, page_no, 612, 792);

  add_text(&document, "#/body", docv1::BaseTextItem::kTitle, docv1::DOC_ITEM_LABEL_TITLE,
           "Quarterly Report");
  add_prov(document.mutable_texts(0)->mutable_title()->mutable_base()->mutable_prov(), 1, 50, 40,
           560, 80);
  add_text(&document, "#/body", docv1::BaseTextItem::kSectionHeader,
           docv1::DOC_ITEM_LABEL_SECTION_HEADER, "Overview", 1);
  add_paragraph(&document, "#/body", "Plain body text with an & and a < in it.");
  add_prov(document.mutable_texts(2)->mutable_text()->mutable_base()->mutable_prov(), 2, 50, 100,
           560, 140);

  const std::string list = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  add_text(&document, list, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "alpha");
  const std::string nested = add_group(&document, list, docv1::GROUP_LABEL_ORDERED_LIST);
  add_text(&document, nested, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "one", 0, true);
  // A list item parented at the body, which the load normalization re-homes.
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "stray");

  auto* table = add_table(&document, "#/body");
  table->add_captions()->set_ref(
      add_owned_text(&document, table->self_ref(), docv1::DOC_ITEM_LABEL_CAPTION, "Fuel table"));
  auto* data = table->mutable_data();
  data->set_num_rows(2);
  data->set_num_cols(2);
  add_cell(data, nullptr, "Fuel", true, 0, 0, 1, 2);
  add_cell(data, nullptr, "Diesel", false, 1, 0);
  add_cell(data, nullptr, "0.9", false, 1, 1);
  add_prov(table->mutable_prov(), 2, 50, 200, 560, 300);

  auto* picture = add_picture(&document, "#/body", "figs/one.png");
  picture->add_captions()->set_ref(
      add_owned_text(&document, picture->self_ref(), docv1::DOC_ITEM_LABEL_CAPTION, "A chart"));
  picture->mutable_meta()->mutable_description()->set_text("a bar chart of sales");
  auto* prediction = picture->mutable_meta()->mutable_classification()->add_predictions();
  prediction->set_class_name("bar_chart");
  prediction->set_confidence(0.75);
  add_prov(picture->mutable_prov(), 3, 60, 60, 300, 260);

  add_code(&document, "#/body", "print('hi')", docv1::CODE_LANGUAGE_LABEL_PYTHON);
  add_text(&document, "#/body", docv1::BaseTextItem::kFormula, docv1::DOC_ITEM_LABEL_FORMULA,
           "E = mc^2");
  add_text(&document, "#/body", docv1::BaseTextItem::kText, docv1::DOC_ITEM_LABEL_FOOTNOTE,
           "a footnote");

  // A track-timed item so the WebVTT export has a cue of its own.
  add_paragraph(&document, "#/body", "spoken line");
  auto* spoken = document.mutable_texts(document.texts_size() - 1)
                     ->mutable_text()
                     ->mutable_base();
  auto* track = spoken->add_source()->mutable_track();
  track->set_start_time(1.0);
  track->set_end_time(2.5);
  track->set_voice("Alice");

  // Custom meta fields, a second unordered map, with names that force the
  // rename pass to pick an order of its own.
  auto& fields = *document.mutable_texts(2)->mutable_text()->mutable_base()->mutable_meta()
                      ->mutable_custom_fields();
  fields["zeta__last"].set_string_value("z");
  fields["alpha__first"].set_string_value("a");
  fields["cell ref"].set_string_value("A1");
  fields["cell-ref"].set_string_value("A2");
  return document;
}

void verify_every_renderer_is_a_pure_function_of_its_document() {
  const docv1::Document document = every_item_document();
  for (const auto& [name, render] : renderers()) {
    const std::string first = render(document);
    const std::string second = render(document);
    require(!first.empty(), name + " must render something for a document with every item kind");
    require_equal(first == second, true,
                  name + " must render the same document to the same bytes twice");
    const std::string third = render(document);
    require_equal(first == third, true,
                  name + " must stay stable past the second render");
  }
}

void verify_two_equal_documents_render_the_same_bytes() {
  const docv1::Document first_build = every_item_document();
  const docv1::Document second_build = every_item_document();
  require(google::protobuf::util::MessageDifferencer::Equals(first_build, second_build),
          "the fixture itself is built the same way twice");
  for (const auto& [name, render] : renderers()) {
    require_equal(render(first_build) == render(second_build), true,
                  name + " must not depend on where the document was allocated");
  }
}

void verify_no_renderer_mutates_the_document_it_reads() {
  const docv1::Document before = every_item_document();
  docv1::Document document = before;
  for (const auto& [name, render] : renderers()) {
    const std::string rendered = render(document);
    require(!rendered.empty(), name + " must render something");
    require(google::protobuf::util::MessageDifferencer::Equals(document, before),
            name + " must leave the document it was handed untouched");
  }
}

void verify_an_empty_document_renders_the_same_way_twice() {
  const docv1::Document document = base_document("empty.pdf");
  for (const auto& [name, render] : renderers()) {
    require_equal(render(document) == render(document), true,
                  name + " must render an empty document the same way twice");
  }
}

}  // namespace

int main() {
  return grparse_test::run_test_main("render-determinism-test", "ok", {
      verify_every_renderer_is_a_pure_function_of_its_document,
      verify_two_equal_documents_render_the_same_bytes,
      verify_no_renderer_mutates_the_document_it_reads,
      verify_an_empty_document_renders_the_same_way_twice,
  });
}
