// The DocTags export, unit by unit: the wrapper, the label token vocabulary,
// heading levels, the code language token, list assembly, OTSL empty cells,
// the picture payloads (classification, molecule, chart table), the
// key-value region, and what a location token needs before it appears.
// Whole-document parity for the same renderer lives in
// document_render_test.cpp; these cases pin the pieces that file does not
// reach.

#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_render.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;

using grparse::render_doctags;
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

// The one line a single-item document renders between the wrapper tags.
std::string only_part(const docv1::Document& document) {
  const std::string doctags = render_doctags(document);
  const std::string open = "<doctag>";
  const std::string close = "\n</doctag>";
  require(doctags.starts_with(open) && doctags.ends_with(close),
          "the export must be wrapped in <doctag>:\n" + doctags);
  return doctags.substr(open.size(), doctags.size() - open.size() - close.size());
}

void verify_an_empty_document_is_the_bare_wrapper() {
  require_equal(render_doctags(base_document("empty.pdf")), "<doctag>\n</doctag>",
                "an empty document renders the wrapper and its delimiter");
}

void verify_heading_levels_never_fall_below_one() {
  docv1::Document document = base_document("headings.pdf");
  add_text(&document, "#/body", docv1::BaseTextItem::kSectionHeader,
           docv1::DOC_ITEM_LABEL_SECTION_HEADER, "unset", 0);
  add_text(&document, "#/body", docv1::BaseTextItem::kSectionHeader,
           docv1::DOC_ITEM_LABEL_SECTION_HEADER, "third", 3);
  require_equal(only_part(document),
                "<section_header_level_1>unset</section_header_level_1>\n"
                "<section_header_level_3>third</section_header_level_3>",
                "an unset level counts as one and a stated level is kept");
}

void verify_the_label_vocabulary_decides_the_token() {
  const struct {
    docv1::DocItemLabel label;
    std::string token;
    std::string what;
  } cases[] = {
      {docv1::DOC_ITEM_LABEL_TEXT, "text", "plain text keeps its own token"},
      {docv1::DOC_ITEM_LABEL_FOOTNOTE, "footnote", "a footnote keeps its own token"},
      {docv1::DOC_ITEM_LABEL_CAPTION, "caption", "a loose caption keeps its own token"},
      {docv1::DOC_ITEM_LABEL_PAGE_HEADER, "page_header", "a page header keeps its own token"},
      {docv1::DOC_ITEM_LABEL_REFERENCE, "reference", "a reference keeps its own token"},
      {docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED, "checkbox_unselected",
       "an unselected checkbox keeps its own token"},
      {docv1::DOC_ITEM_LABEL_CHART, "text", "a label outside the vocabulary degrades to text"},
      {docv1::DOC_ITEM_LABEL_GRADING_SCALE, "text",
       "another label outside the vocabulary degrades to text"},
  };
  for (const auto& one : cases) {
    docv1::Document document = base_document("labels.pdf");
    add_text(&document, "#/body", docv1::BaseTextItem::kText, one.label, "body");
    require_equal(only_part(document), "<" + one.token + ">body</" + one.token + ">", one.what);
  }
}

void verify_item_text_is_trimmed() {
  docv1::Document document = base_document("trim.pdf");
  add_paragraph(&document, "#/body", "  spaced out \n");
  require_equal(only_part(document), "<text>spaced out</text>",
                "surrounding whitespace is trimmed off an item's text");
}

void verify_the_code_language_token_spells_the_tag() {
  docv1::Document plus = base_document("code.md");
  add_code(&plus, "#/body", "int main() {}", docv1::CODE_LANGUAGE_LABEL_C_PLUS_PLUS);
  require_equal(only_part(plus), "<code><_C++_>int main() {}</code>",
                "the punctuation languages are spelled out, not lower-cased");

  docv1::Document unset = base_document("code.md");
  add_code(&unset, "#/body", "x", docv1::CODE_LANGUAGE_LABEL_UNSPECIFIED);
  require_equal(only_part(unset), "<code><_unknown_>x</code>",
                "an unset language reads as unknown");

  docv1::Document raw = base_document("code.md");
  add_code(&raw, "#/body", "x", docv1::CODE_LANGUAGE_LABEL_PYTHON);
  raw.mutable_texts(0)->mutable_code()->set_code_language_raw("jinja");
  require_equal(only_part(raw), "<code><_jinja_>x</code>",
                "the collector's raw language string outranks the enum");

  docv1::Document plain = base_document("code.md");
  add_code(&plain, "#/body", "  spaced  ", docv1::CODE_LANGUAGE_LABEL_GO);
  require_equal(only_part(plain), "<code><_Go_>  spaced  </code>",
                "code keeps its own whitespace");
}

void verify_a_stray_list_item_is_a_one_item_list() {
  docv1::Document document = base_document("list.md");
  add_text(&document, "#/body", docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "orphan");
  require_equal(only_part(document),
                "<unordered_list><list_item>orphan</list_item>\n</unordered_list>",
                "a list item outside a list group still reads as a list");
}

void verify_an_empty_list_group_renders_nothing() {
  docv1::Document document = base_document("list.md");
  add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  require_equal(render_doctags(document), "<doctag>\n</doctag>",
                "a list group with no items contributes no part");
}

void verify_a_transparent_group_joins_its_children_like_top_level_parts() {
  docv1::Document document = base_document("chapter.epub");
  const std::string chapter = add_group(&document, "#/body", docv1::GROUP_LABEL_CHAPTER);
  add_paragraph(&document, chapter, "one");
  add_paragraph(&document, chapter, "two");
  require_equal(only_part(document), "<text>one</text>\n<text>two</text>",
                "a chapter group adds no token of its own");
}

void verify_an_inline_group_gets_its_own_wrapper() {
  docv1::Document document = base_document("inline.docx");
  const std::string group = add_group(&document, "#/body", docv1::GROUP_LABEL_INLINE);
  add_paragraph(&document, group, "run one");
  add_paragraph(&document, group, "run two");
  require_equal(only_part(document), "<inline><text>run one</text><text>run two</text></inline>",
                "an inline group wraps its children in one token");
}

void verify_an_empty_table_cell_is_an_empty_cell_token() {
  docv1::Document document = base_document("cells.pdf");
  auto* table = add_table(&document, "#/body");
  auto* data = table->mutable_data();
  data->set_num_rows(1);
  data->set_num_cols(3);
  add_cell(data, nullptr, "left", false, 0, 0);
  add_cell(data, nullptr, "   ", false, 0, 1);

  require_equal(only_part(document), "<otsl><fcel>left<ecel><ecel><nl></otsl>",
                "a whitespace-only cell and a position no cell reaches are both empty cells");
}

void verify_a_table_with_no_cells_and_no_caption_renders_nothing() {
  docv1::Document document = base_document("cells.pdf");
  add_table(&document, "#/body");
  require_equal(render_doctags(document), "<doctag>\n</doctag>",
                "a table with nothing in it contributes no part");
}

void verify_a_picture_class_promotes_a_chart() {
  docv1::Document chart = base_document("chart.pdf");
  auto* picture = add_picture(&chart, "#/body", "");
  auto* prediction = picture->mutable_meta()->mutable_classification()->add_predictions();
  prediction->set_class_name("pie_chart");
  prediction->set_confidence(0.9);
  require_equal(only_part(chart), "<chart><pie_chart></chart>",
                "a chart class promotes the picture to a chart token");

  docv1::Document logo = base_document("logo.pdf");
  auto* other = add_picture(&logo, "#/body", "");
  other->mutable_meta()->mutable_classification()->add_predictions()->set_class_name("logo");
  require_equal(only_part(logo), "<picture><logo></picture>",
                "a class outside the chart set stays a picture");
}

void verify_the_most_confident_classification_wins() {
  docv1::Document document = base_document("chart.pdf");
  auto* picture = add_picture(&document, "#/body", "");
  auto* classification = picture->mutable_meta()->mutable_classification();
  auto* low = classification->add_predictions();
  low->set_class_name("logo");
  low->set_confidence(0.2);
  auto* high = classification->add_predictions();
  high->set_class_name("bar_chart");
  high->set_confidence(0.8);
  require_equal(only_part(document), "<chart><bar_chart></chart>",
                "the highest-confidence prediction is the picture's class");
}

void verify_a_molecule_rides_a_smiles_token() {
  docv1::Document document = base_document("molecule.pdf");
  add_picture(&document, "#/body", "")->mutable_meta()->mutable_molecule()->set_smi("CCO");
  require_equal(only_part(document), "<picture><smiles>CCO</smiles></picture>",
                "a molecule payload rides its own token");
}

void verify_a_picture_with_nothing_to_say_renders_nothing() {
  docv1::Document document = base_document("blank.pdf");
  add_picture(&document, "#/body", "some.png");
  require_equal(render_doctags(document), "<doctag>\n</doctag>",
                "a picture with no location, class, payload, or caption contributes no part");
}

void verify_a_picture_caption_nests_inside_the_picture() {
  docv1::Document document = base_document("captioned.pdf");
  auto* picture = add_picture(&document, "#/body", "fig.png");
  picture->add_captions()->set_ref(
      add_owned_text(&document, picture->self_ref(), docv1::DOC_ITEM_LABEL_CAPTION, " Figure 1 "));
  require_equal(only_part(document), "<picture><caption>Figure 1</caption></picture>",
                "a caption is trimmed and nested inside its picture");
}

void verify_location_tokens_need_a_page_with_a_size() {
  docv1::Document sized = base_document("located.pdf");
  add_page(&sized, 1, 100, 200);
  add_paragraph(&sized, "#/body", "placed");
  add_prov(sized.mutable_texts(0)->mutable_text()->mutable_base()->mutable_prov(), 1, 0, 0, 100,
           200);
  require_equal(only_part(sized), "<text><loc_0><loc_0><loc_499><loc_499>placed</text>",
                "a full-page box fills the location grid and clamps at its last step");

  docv1::Document unsized = base_document("unsized.pdf");
  add_page(&unsized, 1, 0, 0);
  add_paragraph(&unsized, "#/body", "placed");
  add_prov(unsized.mutable_texts(0)->mutable_text()->mutable_base()->mutable_prov(), 1, 0, 0, 10,
           10);
  require_equal(only_part(unsized), "<text>placed</text>",
                "a page with no size states no location");

  docv1::Document unpaged = base_document("unpaged.pdf");
  add_paragraph(&unpaged, "#/body", "placed");
  add_prov(unpaged.mutable_texts(0)->mutable_text()->mutable_base()->mutable_prov(), 7, 0, 0, 10,
           10);
  require_equal(only_part(unpaged), "<text>placed</text>",
                "a page the document never declares states no location");
}

void verify_a_key_value_region_names_its_cells_and_links() {
  docv1::Document document = base_document("form.pdf");
  auto* item = document.add_key_value_items();
  item->set_self_ref("#/key_value_items/0");
  item->mutable_parent()->set_ref("#/body");
  item->set_content_layer(docv1::CONTENT_LAYER_BODY);
  auto* key = item->mutable_graph()->add_cells();
  key->set_label(docv1::GRAPH_CELL_LABEL_KEY);
  key->set_cell_id(0);
  key->set_text(" Name ");
  auto* value = item->mutable_graph()->add_cells();
  value->set_label(docv1::GRAPH_CELL_LABEL_VALUE);
  value->set_cell_id(1);
  value->set_text("Ada");
  auto* link = item->mutable_graph()->add_links();
  link->set_source_cell_id(0);
  link->set_target_cell_id(1);
  document.mutable_body()->add_children()->set_ref("#/key_value_items/0");

  require_equal(only_part(document),
                "<key_value_region><key_0>Name<link_1></key_0><value_1>Ada</value_1>"
                "</key_value_region>",
                "each cell names its role, its id, and the cells it links to");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("doctags-renderer-test", "ok", {
      verify_an_empty_document_is_the_bare_wrapper,
      verify_heading_levels_never_fall_below_one,
      verify_the_label_vocabulary_decides_the_token,
      verify_item_text_is_trimmed,
      verify_the_code_language_token_spells_the_tag,
      verify_a_stray_list_item_is_a_one_item_list,
      verify_an_empty_list_group_renders_nothing,
      verify_a_transparent_group_joins_its_children_like_top_level_parts,
      verify_an_inline_group_gets_its_own_wrapper,
      verify_an_empty_table_cell_is_an_empty_cell_token,
      verify_a_table_with_no_cells_and_no_caption_renders_nothing,
      verify_a_picture_class_promotes_a_chart,
      verify_the_most_confident_classification_wins,
      verify_a_molecule_rides_a_smiles_token,
      verify_a_picture_with_nothing_to_say_renders_nothing,
      verify_a_picture_caption_nests_inside_the_picture,
      verify_location_tokens_need_a_page_with_a_size,
      verify_a_key_value_region_names_its_cells_and_links,
  });
}
