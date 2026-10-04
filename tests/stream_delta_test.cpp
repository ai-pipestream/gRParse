// Proves a streaming client can rebuild the server's Document from page
// events plus the repair event: the projection carries groups so the tree
// survives the page split, the fold recovers the arenas and the top-level
// children, the rename log composes the repair pass's renumbering, and the
// delta from a fold to the repaired Document applies back to exactly that
// Document, both for a collector's Document and for the CV pipeline's
// pages that the stream groups and repairs at the end.

#include <map>
#include <print>
#include <string>
#include <vector>

#include <google/protobuf/util/message_differencer.h>

#include "grparse/document_assembly.h"
#include "grparse/document_merge.h"
#include "grparse/document_repair.h"
#include "grparse/page_projection.h"
#include "grparse/stream_delta.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

namespace {

using grparse_test::require;

docv1::Document base_document() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  for (int page = 1; page <= 5; ++page) {
    auto& item = (*document.mutable_pages())[page];
    item.set_page_no(page);
    item.mutable_size()->set_width(800.0);
    item.mutable_size()->set_height(1000.0);
  }
  return document;
}

docv1::TextItemBase* add_base(const docv1::Document& document, docv1::TextItemBase* base,
                              const std::string& parent, const std::string& text, int page,
                              double top, double bottom) {
  const std::string ref = "#/texts/" + std::to_string(document.texts_size() - 1);
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref(parent);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  auto* prov = base->add_prov();
  prov->set_page_no(page);
  auto* box = prov->mutable_bbox();
  box->set_l(60.0);
  box->set_t(top);
  box->set_r(740.0);
  box->set_b(bottom);
  box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  return base;
}

std::string add_prose(docv1::Document* document, const std::string& text, int page, double top,
                      double bottom, const std::string& parent = "#/body") {
  auto* base = add_base(*document, document->add_texts()->mutable_text()->mutable_base(), parent,
                        text, page, top, bottom);
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->add_source()->mutable_collector()->set_collector("pdf");
  if (parent == "#/body") document->mutable_body()->add_children()->set_ref(base->self_ref());
  return base->self_ref();
}

std::string add_header(docv1::Document* document, const std::string& text, int page) {
  auto* base = add_base(*document, document->add_texts()->mutable_section_header()->mutable_base(),
                        "#/body", text, page, 100.0, 120.0);
  base->set_label(docv1::DOC_ITEM_LABEL_SECTION_HEADER);
  document->mutable_body()->add_children()->set_ref(base->self_ref());
  return base->self_ref();
}

std::string add_list_item(docv1::Document* document, const std::string& text, int page,
                          double top, const std::string& parent) {
  auto* base = add_base(*document, document->add_texts()->mutable_list_item()->mutable_base(),
                        parent, text, page, top, top + 20.0);
  base->set_label(docv1::DOC_ITEM_LABEL_LIST_ITEM);
  if (parent == "#/body") document->mutable_body()->add_children()->set_ref(base->self_ref());
  return base->self_ref();
}

// Five pages: a running header in the top band of pages 1-4, a paragraph
// a page break splits between pages 1 and 2, a list whose items sit on
// pages 2 and 3, and a table whose comment points at a text after the
// split, so the merge's renumbering reaches a reference outside the tree.
docv1::Document report() {
  docv1::Document document = base_document();
  add_header(&document, "Minutes", 1);
  add_prose(&document, "ACME Quarterly Report", 1, 20.0, 40.0);
  add_prose(&document, "The committee decided that the", 1, 900.0, 920.0);
  add_prose(&document, "ACME Quarterly Report", 2, 20.0, 40.0);
  add_prose(&document, "budget would be approved.", 2, 100.0, 120.0);
  auto* list = document.add_groups();
  list->set_self_ref("#/groups/0");
  list->mutable_parent()->set_ref("#/body");
  list->set_content_layer(docv1::CONTENT_LAYER_BODY);
  list->set_label(docv1::GROUP_LABEL_LIST);
  list->set_name("list");
  document.mutable_body()->add_children()->set_ref("#/groups/0");
  list->add_children()->set_ref(add_list_item(&document, "first point", 2, 700.0, "#/groups/0"));
  document.mutable_groups(0)->add_children()->set_ref(
      add_list_item(&document, "second point", 3, 300.0, "#/groups/0"));
  add_prose(&document, "ACME Quarterly Report", 3, 20.0, 40.0);
  add_prose(&document, "ACME Quarterly Report", 4, 20.0, 40.0);
  const std::string later = add_prose(&document, "Closing remarks.", 4, 300.0, 320.0);
  auto* table = document.add_tables();
  table->set_self_ref("#/tables/0");
  table->mutable_parent()->set_ref("#/body");
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  table->add_prov()->set_page_no(5);
  table->add_comments()->set_ref(later);
  document.mutable_body()->add_children()->set_ref("#/tables/0");
  return document;
}

// The fields page events plus the repair event rebuild.
docv1::Document rebuilt_part(const docv1::Document& document) {
  docv1::Document part;
  *part.mutable_texts() = document.texts();
  *part.mutable_tables() = document.tables();
  *part.mutable_pictures() = document.pictures();
  *part.mutable_groups() = document.groups();
  *part.mutable_body()->mutable_children() = document.body().children();
  *part.mutable_furniture()->mutable_children() = document.furniture().children();
  *part.mutable_pages() = document.pages();
  return part;
}

void require_same_part(const docv1::Document& left, const docv1::Document& right,
                       const std::string& what) {
  google::protobuf::util::MessageDifferencer differencer;
  std::string report;
  differencer.ReportDifferencesToString(&report);
  require(differencer.Compare(rebuilt_part(left), rebuilt_part(right)), what + ": " + report);
}

grparse::PageFold fold_of(const std::vector<parsev1::PageData>& pages) {
  grparse::PageFold fold;
  for (const auto& page : pages) fold.fold(page);
  return fold;
}

// A group rides the page of its first item, and the fold puts the tree
// back together: the list and its items, the top-level body children in
// tree order, the page map. Nothing is left for a repair event to say.
void verify_projection_carries_groups_and_fold_rebuilds_the_tree() {
  const docv1::Document document = report();
  const auto pages = grparse::project_page_data(document, parsev1::TEXT_SOURCE_DIGITAL_PDF);
  require(pages.size() == 5, "five pages project");
  require(pages.at(0).groups_size() == 0 && pages.at(1).groups_size() == 1 &&
              pages.at(1).groups(0).self_ref() == "#/groups/0" && pages.at(2).groups_size() == 0,
          "the list goes out once, on the page of its first item");
  const grparse::PageFold fold = fold_of(pages);
  require_same_part(fold.document(), document, "the fold rebuilds the projected document");
  require(!grparse::repair_delta(fold.document(), document, {}, parsev1::COLLECTOR_PDF)
               .has_value(),
          "a fold that already is the document needs no repair event");
}

void verify_rename_log_composes_steps() {
  grparse::ReferenceRenameLog log;
  docv1::Document scratch;
  // Simultaneous within a step: #/texts/1 retires into #/texts/0 and
  // everything after it moves down one.
  grparse::rewrite_references({{"#/texts/1", "#/texts/0"}, {"#/texts/2", "#/texts/1"},
                               {"#/texts/3", "#/texts/2"}},
                              &scratch);
  // A second step by current names: what is now #/texts/1 (the original
  // #/texts/2) retires into #/texts/0, the rest move down again.
  grparse::rewrite_references({{"#/texts/1", "#/texts/0"}, {"#/texts/2", "#/texts/1"}},
                              &scratch);
  const std::map<std::string, std::string> expected{
      {"#/texts/1", "#/texts/0"}, {"#/texts/2", "#/texts/0"}, {"#/texts/3", "#/texts/1"}};
  require(log.renames() == expected, "two renumberings compose by original ref");
  {
    grparse::ReferenceRenameLog inner;
    grparse::rewrite_references({{"#/texts/1", "#/texts/5"}}, &scratch);
    require(inner.renames().size() == 1, "a nested log sees only its own scope");
  }
  require(log.renames().at("#/texts/3") == "#/texts/5",
          "a nested log hands its renumbering to the enclosing one");
}

// A collector's Document streamed page by page before the repair pass ran:
// the delta from the fold to the repaired Document renames instead of
// resending, and applying it to the fold gives the repaired Document.
void verify_collector_delta_rebuilds_the_repaired_document() {
  const docv1::Document document = report();
  const grparse::PageFold fold =
      fold_of(grparse::project_page_data(document, parsev1::TEXT_SOURCE_DIGITAL_PDF));
  docv1::Document repaired = document;
  std::map<std::string, std::string> renamed;
  grparse::RepairReport report;
  {
    const grparse::ReferenceRenameLog log;
    report = grparse::repair_document(&repaired, grparse::RepairOptions{});
    renamed = log.renames();
  }
  require(report.furniture_demoted == 4, "the running header is demoted on four pages");
  require(report.paragraphs_merged == 1, "the split paragraph rejoins");
  const auto delta =
      grparse::repair_delta(fold.document(), repaired, renamed, parsev1::COLLECTOR_PDF);
  require(delta.has_value(), "the repair pass changed the streamed document");
  require(!delta->renamed_refs().empty(), "the merge's renumbering travels as renames");
  require(delta->texts_size() < repaired.texts_size(),
          "items the pass only renumbered are not resent");
  require(delta->has_furniture_children() && delta->has_body_children(),
          "the demotion changes both trees");
  docv1::Document rebuilt = fold.document();
  require(grparse::apply_repair_delta(*delta, &rebuilt), "the delta applies to the fold");
  require_same_part(rebuilt, repaired, "fold plus delta equals the repaired document");
  require(rebuilt.tables(0).comments(0).ref() == repaired.tables(0).comments(0).ref(),
          "a reference outside the tree follows the renumbering");
}

// The CV pipeline's pages: list items parented to the body, level-less
// headers. The stream applies the terminal event's levels, groups the
// lists and repairs, as the unary path does, and the delta carries the
// result back onto the client's fold.
void verify_cv_pages_delta_matches_the_unary_treatment() {
  docv1::Document cv = base_document();
  add_header(&cv, "Minutes", 1);
  add_prose(&cv, "ACME Quarterly Report", 1, 20.0, 40.0);
  add_prose(&cv, "The committee decided that the", 1, 900.0, 920.0);
  add_prose(&cv, "ACME Quarterly Report", 2, 20.0, 40.0);
  add_prose(&cv, "budget would be approved.", 2, 100.0, 120.0);
  add_list_item(&cv, "first point", 2, 700.0, "#/body");
  add_list_item(&cv, "second point", 3, 300.0, "#/body");
  add_prose(&cv, "ACME Quarterly Report", 3, 20.0, 40.0);
  add_prose(&cv, "ACME Quarterly Report", 4, 20.0, 40.0);
  add_prose(&cv, "Closing remarks.", 4, 300.0, 320.0);
  const grparse::PageFold fold =
      fold_of(grparse::project_page_data(cv, parsev1::TEXT_SOURCE_OCR));
  const std::map<std::string, int32_t> levels{{"#/texts/0", 1}};
  docv1::Document before = fold.document();
  grparse::apply_section_header_levels(levels, &before);
  require(before.texts(0).section_header().level() == 1, "the terminal event's level applies");
  docv1::Document after = before;
  std::map<std::string, std::string> renamed;
  {
    const grparse::ReferenceRenameLog log;
    require(grparse::group_list_items(&after) == 1, "the two list items form one list");
    grparse::repair_document(&after, grparse::RepairOptions{});
    renamed = log.renames();
  }
  const auto delta =
      grparse::repair_delta(before, after, renamed, parsev1::COLLECTOR_GRPARSE_CV);
  require(delta.has_value() && delta->group_count() == 1 && delta->groups_size() == 1,
          "the list group the pages could not carry arrives in the delta");
  docv1::Document rebuilt = fold.document();
  grparse::apply_section_header_levels(levels, &rebuilt);
  require(grparse::apply_repair_delta(*delta, &rebuilt), "the delta applies to the CV fold");
  require_same_part(rebuilt, after, "fold plus delta equals the grouped, repaired document");
}

}  // namespace

int main() {
  verify_projection_carries_groups_and_fold_rebuilds_the_tree();
  verify_rename_log_composes_steps();
  verify_collector_delta_rebuilds_the_repaired_document();
  verify_cv_pages_delta_matches_the_unary_treatment();
  std::println("stream delta tests passed");
  return 0;
}
