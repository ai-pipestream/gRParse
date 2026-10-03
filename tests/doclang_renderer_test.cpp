// The DocLang XML export, unit by unit: the root element, the vocabulary each
// item maps to, list ordinals and nesting depth, table spans and gaps, the
// picture element with and without a description, and the comments the
// unserved arenas leave, the image modes and the namespace switch of
// DoclangOptions, and the DocLang archive's members (OPC parts, assets/,
// pages/) read back out of the ZIP.  Whole-document parity for the same
// renderer lives in document_render_test.cpp; these cases pin the pieces
// that file does not reach.

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <miniz.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "../src/targets/sha256.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/base64.h"
#include "grparse/document_render.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;

using grparse::DoclangOptions;
using grparse::render_dclx;
using grparse::render_doclang;
using grparse_test::add_cell;
using grparse_test::add_code;
using grparse_test::add_group;
using grparse_test::add_heading;
using grparse_test::add_owned_group;
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

const std::string kRoot = "<doclang xmlns=\"http://docling-project.org/ns/doclang/v1\">\n";

DoclangOptions with_mode(DoclangOptions::ImageMode mode) {
  DoclangOptions options;
  options.image_mode = mode;
  return options;
}

// Everything between the root element's tags, so a case states the elements
// it expects and nothing else.  The vocabulary cases state picture uris, so
// they render in the mode that writes the existing one.
std::string body_of(const docv1::Document& document) {
  const std::string doclang =
      render_doclang(document, with_mode(DoclangOptions::ImageMode::kReferenced));
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

// A solid-color PNG of the given pixel size, as the bytes a data URI wraps.
std::string png_of(int width, int height, unsigned char shade) {
  const cv::Mat pixels(height, width, CV_8UC3, cv::Scalar(shade, shade, shade));
  std::vector<unsigned char> png;
  require(cv::imencode(".png", pixels, png), "the fixture PNG must encode");
  return std::string(png.begin(), png.end());
}

std::string data_uri(const std::string& mimetype, const std::string& bytes) {
  return "data:" + mimetype + ";base64," + grparse::encode_base64(bytes.data(), bytes.size());
}

cv::Size png_size(const std::string& png) {
  const cv::Mat buffer(1, static_cast<int>(png.size()), CV_8UC1,
                       const_cast<char*>(png.data()));
  const cv::Mat decoded = cv::imdecode(buffer, cv::IMREAD_UNCHANGED);
  require(!decoded.empty(), "the member must decode as an image");
  return decoded.size();
}

// The archive's members by name, in the order the central directory lists
// them (`names`).
struct Archive {
  std::vector<std::string> names;
  std::map<std::string, std::string> members;
};

Archive unzip(const std::string& bytes) {
  mz_zip_archive zip{};
  require(mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0) != 0,
          "the archive must be a readable ZIP");
  Archive archive;
  const mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint index = 0; index < count; ++index) {
    mz_zip_archive_file_stat stat;
    require(mz_zip_reader_file_stat(&zip, index, &stat) != 0, "member stat");
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, index, &size, 0);
    require(data != nullptr, std::string("member must inflate: ") + stat.m_filename);
    archive.names.emplace_back(stat.m_filename);
    archive.members[stat.m_filename] = std::string(static_cast<const char*>(data), size);
    mz_free(data);
  }
  mz_zip_reader_end(&zip);
  return archive;
}

// The mutable base of a text arena entry the builder wrote as a plain text.
docv1::TextItemBase* text_base_at(docv1::Document* document, const std::string& ref) {
  const std::string prefix = "#/texts/";
  require(ref.starts_with(prefix), "fixture ref must name the text arena");
  auto* item = document->mutable_texts(std::stoi(ref.substr(prefix.size())));
  return item->item_case() == docv1::BaseTextItem::kListItem
             ? item->mutable_list_item()->mutable_base()
             : item->mutable_text()->mutable_base();
}

// A group held by a text item (docling's host shape): it lives in the group
// arena with the host as its parent and is linked as the host's child, never
// under the body.
std::string add_hosted_group(docv1::Document* document, const std::string& host,
                             docv1::GroupLabel label) {
  const std::string ref = add_owned_group(document, host, label);
  text_base_at(document, host)->add_children()->set_ref(ref);
  return ref;
}

// A field region holding one field item whose key and value are its two text
// children, hosted by `host` (a text item).
void add_hosted_field_region(docv1::Document* document, const std::string& host,
                             const std::string& key, const std::string& value) {
  const std::string region_ref =
      "#/field_regions/" + std::to_string(document->field_regions_size());
  const std::string field_ref = "#/field_items/" + std::to_string(document->field_items_size());
  auto* region = document->add_field_regions();
  region->set_self_ref(region_ref);
  region->mutable_parent()->set_ref(host);
  region->set_label(docv1::DOC_ITEM_LABEL_FIELD_REGION);
  region->set_content_layer(docv1::CONTENT_LAYER_BODY);
  region->add_children()->set_ref(field_ref);
  text_base_at(document, host)->add_children()->set_ref(region_ref);
  auto* field = document->add_field_items();
  field->set_self_ref(field_ref);
  field->mutable_parent()->set_ref(region_ref);
  field->set_label(docv1::DOC_ITEM_LABEL_FIELD_ITEM);
  field->set_content_layer(docv1::CONTENT_LAYER_BODY);
  for (const auto& [label, text] : {std::pair{docv1::DOC_ITEM_LABEL_FIELD_KEY, key},
                                    std::pair{docv1::DOC_ITEM_LABEL_FIELD_VALUE, value}}) {
    field->add_children()->set_ref(add_owned_text(document, field_ref, label, text));
  }
}

// docling-core #804's host shape: a footnote (or any text) with no text of
// its own whose first child is an inline group of runs. The runs belong
// inside the host's element, after any text the host has, exactly once;
// later children follow the host as siblings.
void verify_a_host_renders_its_inline_runs_inside_its_element() {
  docv1::Document document = base_document("host.pdf");
  const std::string note = add_text(&document, "#/body", docv1::BaseTextItem::kText,
                                    docv1::DOC_ITEM_LABEL_FOOTNOTE, "");
  const std::string runs = add_hosted_group(&document, note, docv1::GROUP_LABEL_INLINE);
  add_paragraph(&document, runs, "①");
  add_paragraph(&document, runs, "Yanchi county gazetteer");

  const std::string titled = add_text(&document, "#/body", docv1::BaseTextItem::kText,
                                      docv1::DOC_ITEM_LABEL_FOOTNOTE, "See");
  const std::string tail = add_hosted_group(&document, titled, docv1::GROUP_LABEL_INLINE);
  add_paragraph(&document, tail, "p.");
  add_paragraph(&document, tail, "4");
  const std::string after = add_hosted_group(&document, titled, docv1::GROUP_LABEL_UNSPECIFIED);
  add_paragraph(&document, after, "a later child");

  const std::string list = add_group(&document, "#/body", docv1::GROUP_LABEL_LIST);
  const std::string item = add_text(&document, list, docv1::BaseTextItem::kListItem,
                                    docv1::DOC_ITEM_LABEL_LIST_ITEM, "");
  const std::string item_runs = add_hosted_group(&document, item, docv1::GROUP_LABEL_INLINE);
  add_paragraph(&document, item_runs, "bold");
  add_paragraph(&document, item_runs, "lead");
  const std::string sublist = add_hosted_group(&document, item, docv1::GROUP_LABEL_LIST);
  add_text(&document, sublist, docv1::BaseTextItem::kListItem, docv1::DOC_ITEM_LABEL_LIST_ITEM,
           "nested");

  require_equal(body_of(document),
                "  <footnote>① Yanchi county gazetteer</footnote>\n"
                "  <footnote>See p. 4</footnote>\n"
                "  <paragraph>a later child</paragraph>\n"
                "  <list ordered=\"false\">\n"
                "    <list-item>bold lead</list-item>\n"
                "    <list ordered=\"false\">\n"
                "      <list-item>nested</list-item>\n"
                "    </list>\n"
                "  </list>\n",
                "a host's inline runs render inside its element once, after its own text, "
                "and its other children follow it (a list item's nested list inside the list)");
}

// An inline group in the body is one flow of runs, so it folds into one
// paragraph, as the HTML and Google Docs exports fold it, instead of one
// paragraph per run.
void verify_a_body_inline_group_folds_into_one_paragraph() {
  docv1::Document document = base_document("inline.pdf");
  const std::string runs = add_group(&document, "#/body", docv1::GROUP_LABEL_INLINE);
  add_paragraph(&document, runs, "Water is");
  const std::string bold = add_paragraph(&document, runs, "wet");
  text_base_at(&document, bold)->mutable_formatting()->set_bold(true);
  add_paragraph(&document, runs, "");
  add_text(&document, runs, docv1::BaseTextItem::kFormula, docv1::DOC_ITEM_LABEL_FORMULA,
           "H_2O");
  require_equal(body_of(document), "  <paragraph>Water is wet H_2O</paragraph>\n",
                "the runs join with a space into one paragraph, formatting kept as text");
}

// docling-core #824: a table's footnotes render after it, each holding its
// nested content (a field region held by an otherwise empty footnote) inside
// its own element; an empty footnote is omitted, and a footnote the tree also
// links ahead of the table still renders once, with its table.
void verify_float_footnotes_render_after_their_float_with_nested_content() {
  docv1::Document document = base_document("footnotes.pdf");
  const std::string early = add_text(&document, "#/body", docv1::BaseTextItem::kText,
                                     docv1::DOC_ITEM_LABEL_FOOTNOTE, "Plain note.");
  auto* table = add_table(&document, "#/body");
  table->mutable_data()->set_num_rows(1);
  table->mutable_data()->set_num_cols(1);
  add_cell(table->mutable_data(), nullptr, "x", false, 0, 0);
  const std::string table_ref = table->self_ref();
  const std::string region_note =
      add_owned_text(&document, table_ref, docv1::DOC_ITEM_LABEL_FOOTNOTE, "");
  add_hosted_field_region(&document, region_note, "K:", "V");
  const std::string empty_note =
      add_owned_text(&document, table_ref, docv1::DOC_ITEM_LABEL_FOOTNOTE, "");
  // The region is also linked under the body, ahead of the table like the
  // plain footnote; both still render only with the table.
  auto* body = document.mutable_body()->mutable_children();
  body->Add()->set_ref("#/field_regions/0");
  for (int index = body->size() - 1; index > 0; --index) body->SwapElements(index, index - 1);
  auto* footnotes = document.mutable_tables(0)->mutable_footnotes();
  for (const auto& ref : {region_note, early, empty_note}) footnotes->Add()->set_ref(ref);
  add_paragraph(&document, "#/body", "after");

  const std::string expected =
      "  <table>\n"
      "    <tr>\n"
      "      <td>x</td>\n"
      "    </tr>\n"
      "  </table>\n"
      "  <footnote>\n"
      "    <paragraph>K:</paragraph>\n"
      "    <paragraph>V</paragraph>\n"
      "  </footnote>\n"
      "  <footnote>Plain note.</footnote>\n"
      "  <paragraph>after</paragraph>\n";
  require_equal(body_of(document), expected,
                "the footnotes follow their table, nested content inside, each once");
  const Archive read = unzip(render_dclx(document));
  require_equal(read.members.at("document.xml"), kRoot + expected + "</doclang>\n",
                "the archive's document.xml carries the same footnotes");
}

// The same for a figure's captions: a caption holding nested content keeps
// it inside its element (its own text first), a caption with nothing in it
// is omitted, and a picture's footnote follows the picture.
void verify_float_captions_keep_their_nested_content() {
  docv1::Document document = base_document("captions.pdf");
  auto* figure = add_picture(&document, "#/body", "figs/a.png");
  const std::string figure_ref = figure->self_ref();
  const std::string first =
      add_owned_text(&document, figure_ref, docv1::DOC_ITEM_LABEL_CAPTION, "A chart");
  const std::string sourced =
      add_owned_text(&document, figure_ref, docv1::DOC_ITEM_LABEL_CAPTION, "Source:");
  const std::string runs = add_hosted_group(&document, sourced, docv1::GROUP_LABEL_INLINE);
  add_paragraph(&document, runs, "survey");
  const std::string blocks = add_hosted_group(&document, sourced, docv1::GROUP_LABEL_UNSPECIFIED);
  add_paragraph(&document, blocks, "n = 40");
  const std::string empty =
      add_owned_text(&document, figure_ref, docv1::DOC_ITEM_LABEL_CAPTION, "");
  const std::string note =
      add_owned_text(&document, figure_ref, docv1::DOC_ITEM_LABEL_FOOTNOTE, "");
  add_paragraph(&document, add_hosted_group(&document, note, docv1::GROUP_LABEL_INLINE),
                "estimated");
  auto* picture = document.mutable_pictures(0);
  for (const auto& ref : {first, sourced, empty}) picture->add_captions()->set_ref(ref);
  picture->add_footnotes()->set_ref(note);

  require_equal(body_of(document),
                "  <caption>A chart</caption>\n"
                "  <caption>\n"
                "    Source: survey\n"
                "    <paragraph>n = 40</paragraph>\n"
                "  </caption>\n"
                "  <picture uri=\"figs/a.png\"/>\n"
                "  <footnote>estimated</footnote>\n",
                "captions keep their nested content inside their element and the footnote "
                "follows the picture");
}

// Two pages of 100 x 200 units with 50 x 100 pixel page images (half scale),
// then three pictures in body order: one carrying its own PNG, one with no
// image whose box on page 2 must be cropped out of that page's image, and
// one whose image is a relative reference no archive can load.
struct Fixture {
  docv1::Document document;
  std::string own_png;
};

Fixture image_fixture() {
  Fixture fixture{base_document("pictures.pdf"), png_of(7, 5, 40)};
  docv1::Document& document = fixture.document;
  for (const int page_no : {2, 1}) {
    auto* page = add_page(&document, page_no, 100, 200);
    page->mutable_image()->set_mimetype("image/png");
    page->mutable_image()->set_uri(
        data_uri("image/png", png_of(50, 100, static_cast<unsigned char>(page_no * 60))));
  }
  add_picture(&document, "#/body", data_uri("image/png", fixture.own_png));
  auto* cropped = add_picture(&document, "#/body", "");
  // 20..60 x 40..120 units is 10..30 x 20..60 pixels: a 20 x 40 crop.
  add_prov(cropped->mutable_prov(), 2, 20, 40, 60, 120);
  add_picture(&document, "#/body", "figs/remote.png");
  return fixture;
}

void verify_doclang_image_modes() {
  const Fixture fixture = image_fixture();
  const std::string own_uri = data_uri("image/png", fixture.own_png);

  const std::string placeholder = render_doclang(fixture.document);
  require(!placeholder.contains("uri="),
          "the default (placeholder) mode writes no picture source:\n" + placeholder);
  require_equal(placeholder,
                render_doclang(fixture.document,
                               with_mode(DoclangOptions::ImageMode::kPlaceholder)),
                "an unset image mode is the placeholder mode");

  require_equal(render_doclang(fixture.document,
                               with_mode(DoclangOptions::ImageMode::kReferenced)),
                kRoot + "  <picture uri=\"" + own_uri + "\"/>\n  <picture/>\n" +
                    "  <picture uri=\"figs/remote.png\"/>\n</doclang>",
                "referenced writes each picture's existing uri and nothing for one without");

  const std::string embedded =
      render_doclang(fixture.document, with_mode(DoclangOptions::ImageMode::kEmbedded));
  require(embedded.contains("<picture uri=\"" + own_uri + "\"/>"),
          "embedded keeps an existing data URI:\n" + embedded);
  require(embedded.contains("<picture uri=\"figs/remote.png\"/>"),
          "embedded writes an existing non-data uri as is, like docling:\n" + embedded);
  const std::string prefix = "<picture uri=\"data:image/png;base64,";
  const size_t start = embedded.find(prefix, embedded.find(own_uri) + own_uri.size());
  require(start != std::string::npos,
          "embedded crops a picture without an image out of its page image:\n" + embedded);
  const size_t payload = start + prefix.size();
  const std::string crop =
      grparse::decode_base64(embedded.substr(payload, embedded.find('"', payload) - payload));
  require(png_size(crop) == cv::Size(20, 40),
          "the crop is the provenance box scaled from page units to image pixels");
}

// A page image over GRPARSE_MAX_IMAGE_PIXELS is never decoded for a crop:
// its header is checked first, and the picture is written without a source.
void verify_embedded_crop_honors_pixel_cap() {
  const Fixture fixture = image_fixture();
  setenv("GRPARSE_MAX_IMAGE_PIXELS", "100", 1);
  const std::string embedded =
      render_doclang(fixture.document, with_mode(DoclangOptions::ImageMode::kEmbedded));
  unsetenv("GRPARSE_MAX_IMAGE_PIXELS");
  const std::string own_uri = data_uri("image/png", fixture.own_png);
  require(embedded.contains("<picture uri=\"" + own_uri + "\"/>\n  <picture/>\n"),
          "the over-cap page image yields no crop:\n" + embedded);
}

void verify_doclang_namespace_switch() {
  const docv1::Document document = base_document("ns.pdf");
  require_equal(render_doclang(document), kRoot + "</doclang>",
                "the namespace is declared by default");
  DoclangOptions bare;
  bare.include_namespace = false;
  require_equal(render_doclang(document, bare), std::string("<doclang>\n</doclang>"),
                "include_namespace=false writes a bare root");
}

void verify_dclx_is_a_zip_with_document_xml() {
  const docv1::Document document = base_document("archive.pdf");
  const std::string archive = render_dclx(document);
  require(archive.size() >= 4 && archive[0] == 'P' && archive[1] == 'K',
          "render_dclx must produce a ZIP");
  const Archive read = unzip(archive);
  require_equal(read.names.size(), std::size_t{3}, "an imageless document packs three members");
  require_equal(read.names[0], std::string("[Content_Types].xml"), "OPC content types first");
  require_equal(read.names[1], std::string("_rels/.rels"), "then the package relationships");
  require_equal(read.names[2], std::string("document.xml"), "then the document");
  require(read.members.at("[Content_Types].xml")
              .contains("PartName=\"/document.xml\" "
                        "ContentType=\"application/vnd.doclang.document+xml\""),
          "the content types name the DocLang document part");
  require(read.members.at("_rels/.rels").contains("Target=\"document.xml\""),
          "the package relationship targets document.xml");
  require_equal(read.members.at("document.xml"), kRoot + "</doclang>\n",
                "document.xml is the export plus a final newline, as docling writes it");
  require_equal(archive, render_dclx(document), "render_dclx must be deterministic");
}

void verify_dclx_referenced_stores_assets_and_pages() {
  const Fixture fixture = image_fixture();
  const std::string archive = render_dclx(fixture.document);
  require_equal(archive,
                render_dclx(fixture.document, with_mode(DoclangOptions::ImageMode::kReferenced)),
                "the archive defaults to the referenced mode");
  const Archive read = unzip(archive);

  const std::string own_asset =
      "assets/image_000000_" + grparse::targets::sha256_hex(fixture.own_png) + ".png";
  require(read.members.contains(own_asset), "a picture's own image is stored as " + own_asset);
  require_equal(read.members.at(own_asset), fixture.own_png,
                "a PNG picture image is stored byte for byte");

  std::string crop_asset;
  for (const auto& name : read.names) {
    if (name.starts_with("assets/image_000001_")) crop_asset = name;
  }
  require(!crop_asset.empty() && crop_asset.ends_with(".png"),
          "the picture without an image is stored as the second asset");
  require(png_size(read.members.at(crop_asset)) == cv::Size(20, 40),
          "the second asset is the crop of its page image");

  const std::string& xml = read.members.at("document.xml");
  require(xml.contains("<picture uri=\"" + own_asset + "\"/>") &&
              xml.contains("<picture uri=\"" + crop_asset + "\"/>"),
          "each stored picture references its asset:\n" + xml);
  require(xml.contains("<picture uri=\"figs/remote.png\"/>"),
          "an image the archive cannot load keeps its uri:\n" + xml);
  require(!xml.contains("data:"), "no image rides inside the archive's markup:\n" + xml);

  require(read.members.contains("pages/1.png") && read.members.contains("pages/2.png"),
          "every page image is stored under pages/");
  require(png_size(read.members.at("pages/2.png")) == cv::Size(50, 100),
          "a page image is stored as it came");

  std::vector<std::string> sorted = read.names;
  std::ranges::sort(sorted);
  require(sorted == read.names, "members are written in path order");
  require_equal(read.names.size(), std::size_t{7},
                "content types, rels, two assets, document.xml and two pages");
  require_equal(archive, render_dclx(fixture.document), "the archive is deterministic");
}

void verify_dclx_placeholder_keeps_pages_only() {
  const Fixture fixture = image_fixture();
  const Archive read =
      unzip(render_dclx(fixture.document, with_mode(DoclangOptions::ImageMode::kPlaceholder)));
  for (const auto& name : read.names) {
    require(!name.starts_with("assets/"), "placeholder stores no picture image: " + name);
  }
  require(read.members.contains("pages/1.png") && read.members.contains("pages/2.png"),
          "page images are stored in the placeholder mode too");
  require(!read.members.at("document.xml").contains("uri="),
          "placeholder references no picture image:\n" + read.members.at("document.xml"));
}

void verify_dclx_rejects_embedded() {
  bool rejected = false;
  try {
    render_dclx(base_document("embedded.pdf"), with_mode(DoclangOptions::ImageMode::kEmbedded));
  } catch (const std::invalid_argument& error) {
    rejected = std::string(error.what()).contains("EMBEDDED");
  }
  require(rejected, "the archive rejects the embedded mode, as docling does");
}

void verify_dclx_namespace_switch() {
  DoclangOptions bare;
  bare.include_namespace = false;
  const Archive read = unzip(render_dclx(base_document("ns.pdf"), bare));
  require_equal(read.members.at("document.xml"), std::string("<doclang>\n</doclang>\n"),
                "include_namespace=false reaches the archive's document.xml");
}

void verify_dclx_transcodes_other_formats_to_png() {
  docv1::Document document = base_document("bitmap.pdf");
  const cv::Mat pixels(3, 4, CV_8UC3, cv::Scalar(10, 20, 30));
  std::vector<unsigned char> bitmap;
  require(cv::imencode(".bmp", pixels, bitmap), "the fixture BMP must encode");
  add_picture(&document, "#/body",
              data_uri("image/bmp", std::string(bitmap.begin(), bitmap.end())));
  const Archive read = unzip(render_dclx(document));
  std::string asset;
  for (const auto& name : read.names) {
    if (name.starts_with("assets/")) asset = name;
  }
  require(asset.starts_with("assets/image_000000_") && asset.ends_with(".png"),
          "a format the content types do not declare is stored as PNG: " + asset);
  require(png_size(read.members.at(asset)) == cv::Size(4, 3), "the re-encoded pixels survive");
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
      verify_a_host_renders_its_inline_runs_inside_its_element,
      verify_a_body_inline_group_folds_into_one_paragraph,
      verify_float_footnotes_render_after_their_float_with_nested_content,
      verify_float_captions_keep_their_nested_content,
      verify_doclang_image_modes,
      verify_doclang_namespace_switch,
      verify_dclx_is_a_zip_with_document_xml,
      verify_dclx_referenced_stores_assets_and_pages,
      verify_dclx_placeholder_keeps_pages_only,
      verify_dclx_rejects_embedded,
      verify_dclx_namespace_switch,
      verify_dclx_transcodes_other_formats_to_png,
      verify_embedded_crop_honors_pixel_cap,
  });
}
