// S3 eval findings against the integrity walk: a markup item located by
// source line (ProvenanceItem.line_range, page-less by design) was reported
// as sitting on page 0, and a text-layer PDF's link spans pointing at
// "#/pages/N" (a page destination the outline uses the same way) were
// reported as unresolved. Both are legitimate shapes the walk must accept;
// a destination on a page the document does not have stays an error.

#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/docling_map.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

docv1::TextItemBase* add_paragraph(docv1::Document* document, const std::string& text) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  docv1::TextItemBase* base = document->add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->set_text(text);
  document->mutable_body()->add_children()->set_ref(ref);
  return base;
}

docv1::Document skeleton() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_furniture()->set_self_ref("#/furniture");
  return document;
}

std::string joined(const std::vector<std::string>& errors) {
  std::string out;
  for (const auto& error : errors) out += error + "; ";
  return out;
}

void verify_line_addressed_provenance_is_page_less() {
  docv1::Document document = skeleton();
  docv1::TextItemBase* base = add_paragraph(&document, "A markdown paragraph");
  docv1::ProvenanceItem* prov = base->add_prov();
  prov->mutable_line_range()->set_start(12);
  prov->mutable_line_range()->set_end(13);
  const auto errors = grparse::docling_integrity_errors(document);
  require(errors.empty(), "a line-addressed item is not on page 0: " + joined(errors));
}

void verify_page_destinations_resolve() {
  docv1::Document document = skeleton();
  (*document.mutable_pages())[1].set_page_no(1);
  (*document.mutable_pages())[3].set_page_no(3);
  docv1::TextItemBase* base = add_paragraph(&document, "See page three");
  docv1::ProvenanceItem* prov = base->add_prov();
  prov->set_page_no(1);
  docv1::InlineSpan* span = base->add_spans();
  span->mutable_range()->set_start(4);
  span->mutable_range()->set_end(14);
  span->mutable_target()->set_ref("#/pages/3");
  docv1::OutlineEntry* entry = document.add_outline();
  entry->set_title("Three");
  entry->mutable_target()->set_ref("#/pages/3");
  require(grparse::docling_integrity_errors(document).empty(),
          "a span or outline target on an existing page resolves");

  span->mutable_target()->set_ref("#/pages/9");
  const auto errors = grparse::docling_integrity_errors(document);
  require(errors.size() == 1 && errors[0].find("#/pages/9") != std::string::npos,
          "a destination on a page the document lacks is still an error: " + joined(errors));
}

void verify_a_page_plane_item_still_needs_a_page() {
  docv1::Document document = skeleton();
  docv1::TextItemBase* base = add_paragraph(&document, "boxed but pageless");
  docv1::ProvenanceItem* prov = base->add_prov();
  prov->mutable_bbox()->set_r(10);
  prov->mutable_bbox()->set_b(10);
  const auto errors = grparse::docling_integrity_errors(document);
  require(errors.size() == 1 && errors[0].find("not a 1-based page") != std::string::npos,
          "a box with no page and no other locator is still page 0: " + joined(errors));
}

}  // namespace

int main() {
  try {
    verify_line_addressed_provenance_is_page_less();
    verify_page_destinations_resolve();
    verify_a_page_plane_item_still_needs_a_page();
  } catch (const std::exception& error) {
    std::println(stderr, "docling_integrity_destinations_test: {}", error.what());
    return 1;
  }
  std::println("docling_integrity_destinations_test: ok");
  return 0;
}
