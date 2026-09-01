#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>

#include "grparse/page_projection.h"
#include "support/check.h"

namespace {

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;
using grparse::project_page_data;

using grparse_test::require;

docv1::TextItemBase* add_text(docv1::Document* document, const std::string& ref,
                              const std::string& text, int page, bool body = true) {
  auto* base = document->add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref(body ? "#/body" : "#/furniture");
  base->set_content_layer(body ? docv1::CONTENT_LAYER_BODY : docv1::CONTENT_LAYER_FURNITURE);
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->set_text(text);
  if (page > 0) base->add_prov()->set_page_no(page);
  (body ? document->mutable_body() : document->mutable_furniture())->add_children()->set_ref(ref);
  return base;
}

// A document that never names a page projects to nothing: the caller keeps
// its single whole-document event and no empty page is invented.
void verify_pageless_document_projects_nothing() {
  docv1::Document document;
  add_text(&document, "#/texts/0", "no page", 0);
  require(project_page_data(document, parsev1::TEXT_SOURCE_UNSPECIFIED).empty(),
          "a page-less document must not project pages");
}

// Reading order is the body tree, groups included; furniture follows the
// body on its page; a page-less item rides the page of its predecessor;
// pages nobody placed an item on still exist, empty, so numbering is dense.
void verify_tree_order_and_page_inheritance() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_furniture()->set_self_ref("#/furniture");
  add_text(&document, "#/texts/0", "header", 1, /*body=*/false);
  add_text(&document, "#/texts/1", "first", 1);
  // A list group on page 1 whose member sits in the texts arena after the
  // page-3 item: the walk, not arena order, decides the sequence.
  auto* group = document.add_groups();
  group->set_self_ref("#/groups/0");
  group->mutable_parent()->set_ref("#/body");
  group->set_label(docv1::GROUP_LABEL_LIST);
  document.mutable_body()->add_children()->set_ref("#/groups/0");
  add_text(&document, "#/texts/2", "third page", 3);
  auto* member = document.add_texts()->mutable_list_item()->mutable_base();
  member->set_self_ref("#/texts/3");
  member->mutable_parent()->set_ref("#/groups/0");
  member->set_label(docv1::DOC_ITEM_LABEL_LIST_ITEM);
  member->set_text("member");
  member->add_prov()->set_page_no(1);
  group->add_children()->set_ref("#/texts/3");
  // Page-less picture after the page-3 text inherits page 3.
  auto* picture = document.add_pictures();
  picture->set_self_ref("#/pictures/0");
  picture->mutable_parent()->set_ref("#/body");
  document.mutable_body()->add_children()->set_ref("#/pictures/0");

  const auto pages = project_page_data(document, parsev1::TEXT_SOURCE_DIGITAL_PDF);
  require(pages.size() == 3, "pages run densely from 1 to the highest page named");
  require(pages.at(0).page_number() == 1 && pages.at(1).page_number() == 2 &&
              pages.at(2).page_number() == 3,
          "page numbers are ordinal");
  require(pages.at(0).page_meta().page_no() == 1, "page metadata names its page");
  require(pages.at(1).texts_size() == 0 && pages.at(1).pictures_size() == 0,
          "an unplaced page is empty, not missing");
  const auto& one = pages.at(0);
  require(one.texts_size() == 3, "page 1 holds the body items and then the furniture");
  require(one.texts(0).text().base().self_ref() == "#/texts/1" &&
              one.texts(1).list_item().base().self_ref() == "#/texts/3" &&
              one.texts(2).text().base().self_ref() == "#/texts/0",
          "body tree order first, group member in place, furniture after");
  require(one.body_order_size() == 2 && one.body_order(0).ref() == "#/texts/1" &&
              one.body_order(1).ref() == "#/texts/3",
          "body order excludes furniture");
  require(one.text_offsets_size() == 3 && one.text_offsets(0).utf_start() == 0 &&
              one.text_offsets(0).utf_end() == 5 && one.text_offsets(1).utf_start() == 5 &&
              one.text_offsets(1).utf_end() == 11 &&
              one.text_offsets(0).source() == parsev1::TEXT_SOURCE_DIGITAL_PDF,
          "offsets accumulate in emission order with the caller's source");
  const auto& three = pages.at(2);
  require(three.texts_size() == 1 && three.pictures_size() == 1 &&
              three.pictures(0).self_ref() == "#/pictures/0",
          "the page-less picture rides the page of the item before it");
  require(three.text_offsets(0).utf_start() == 11, "offsets continue across pages");
}

// Page metadata the collector measured is carried whole; the projection
// only stamps the page number.
void verify_page_map_metadata_is_carried() {
  docv1::Document document;
  add_text(&document, "#/texts/0", "x", 2);
  auto& page = (*document.mutable_pages())[2];
  page.mutable_size()->set_width(595);
  page.mutable_size()->set_height(842);
  page.set_unit("pt");
  const auto pages = project_page_data(document, parsev1::TEXT_SOURCE_UNSPECIFIED);
  require(pages.size() == 2, "page 1 exists empty before the placed page 2");
  require(pages.at(1).page_meta().size().height() == 842 && pages.at(1).page_meta().unit() == "pt" &&
              pages.at(1).page_meta().page_no() == 2,
          "the collector's page item rides the event untouched");
}

// Items no tree reaches are not lost: they follow the reachable ones on
// their own page, body-ordered only when they say they are body.
void verify_orphans_are_placed() {
  docv1::Document document;
  add_text(&document, "#/texts/0", "reached", 1);
  auto* orphan = document.add_texts()->mutable_text()->mutable_base();
  orphan->set_self_ref("#/texts/1");
  orphan->set_content_layer(docv1::CONTENT_LAYER_BODY);
  orphan->set_text("orphan");
  orphan->add_prov()->set_page_no(1);
  const auto pages = project_page_data(document, parsev1::TEXT_SOURCE_UNSPECIFIED);
  require(pages.size() == 1 && pages.at(0).texts_size() == 2 &&
              pages.at(0).texts(1).text().base().self_ref() == "#/texts/1" &&
              pages.at(0).body_order_size() == 2,
          "the orphan lands on its page after the reachable items");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("page-projection-test", {
      verify_pageless_document_projects_nothing,
      verify_tree_order_and_page_inheritance,
      verify_page_map_metadata_is_carried,
      verify_orphans_are_placed,
  });
}
