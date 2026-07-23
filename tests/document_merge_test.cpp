#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/docling_map.h"
#include "grparse/document_merge.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

docv1::Document base_document() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  return document;
}

void add_text(docv1::Document* document, const std::string& parent,
              const std::string& text) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* base = document->add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref(parent);
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  if (parent == "#/body") {
    document->mutable_body()->add_children()->set_ref(ref);
  }
}

// A collector-shaped source document: a group holding a text, a table whose
// comment points at the text, and a page. Every reference must survive the
// renumbering.
docv1::Document collector_document() {
  docv1::Document source = base_document();
  auto* group = source.add_groups();
  group->set_self_ref("#/groups/0");
  group->mutable_parent()->set_ref("#/body");
  group->set_label(docv1::GROUP_LABEL_SHEET);
  source.mutable_body()->add_children()->set_ref("#/groups/0");

  auto* base = source.add_texts()->mutable_text()->mutable_base();
  base->set_self_ref("#/texts/0");
  base->mutable_parent()->set_ref("#/groups/0");
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->set_text("collected text");
  group->add_children()->set_ref("#/texts/0");

  auto* table = source.add_tables();
  table->set_self_ref("#/tables/0");
  table->mutable_parent()->set_ref("#/body");
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  table->add_comments()->set_ref("#/texts/0");
  source.mutable_body()->add_children()->set_ref("#/tables/0");

  (*source.mutable_pages())[2].set_page_no(2);
  (*source.mutable_body()->mutable_meta()->mutable_custom_fields())["named_range:R"]
      .set_string_value("A1:B2");
  return source;
}

void verify_merge_renumbers_and_rewrites() {
  docv1::Document target = base_document();
  add_text(&target, "#/body", "existing text");
  auto* existing_table = target.add_tables();
  existing_table->set_self_ref("#/tables/0");
  existing_table->mutable_parent()->set_ref("#/body");
  existing_table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  target.mutable_body()->add_children()->set_ref("#/tables/0");
  (*target.mutable_pages())[1].set_page_no(1);

  grparse::merge_documents(collector_document(), &target);

  require(target.texts_size() == 2 && target.tables_size() == 2 &&
              target.groups_size() == 1,
          "merge appends every arena");
  require(target.texts(0).text().base().self_ref() == "#/texts/0" &&
              target.texts(0).text().base().text() == "existing text",
          "existing items stay untouched");
  const auto& moved = target.texts(1).text().base();
  require(moved.self_ref() == "#/texts/1" && moved.text() == "collected text",
          "moved text is renumbered past the existing arena");
  require(moved.parent().ref() == "#/groups/0",
          "moved text keeps its parent group");
  require(target.groups(0).children(0).ref() == "#/texts/1",
          "group child references follow the renumbering");
  require(target.tables(1).self_ref() == "#/tables/1",
          "moved table is renumbered past the existing table");
  require(target.tables(1).comments(0).ref() == "#/texts/1",
          "fine references follow the renumbering");
  require(target.body().children_size() == 4 &&
              target.body().children(2).ref() == "#/groups/0" &&
              target.body().children(3).ref() == "#/tables/1",
          "body children append with rewritten references");
  require(target.pages().size() == 2 && target.pages().at(2).page_no() == 2,
          "pages merge by page number");
  require(target.body().meta().custom_fields().count("named_range:R") == 1,
          "body metadata merges additively");
  const auto errors = grparse::docling_integrity_errors(target);
  for (const auto& error : errors) std::cerr << "integrity: " << error << '\n';
  require(errors.empty(), "merged document ref tree stays well formed");
}

// Merging the same collector output twice keeps both copies addressable:
// additive means never overwriting, even on replay.
void verify_merge_is_additive_on_replay() {
  docv1::Document target = base_document();
  grparse::merge_documents(collector_document(), &target);
  grparse::merge_documents(collector_document(), &target);
  require(target.texts_size() == 2 && target.groups_size() == 2,
          "replayed merge appends instead of overwriting");
  require(target.texts(1).text().base().self_ref() == "#/texts/1" &&
              target.texts(1).text().base().parent().ref() == "#/groups/1",
          "second copy renumbers into its own refs");
  require(grparse::docling_integrity_errors(target).empty(),
          "replayed merge stays well formed");
}

}  // namespace

int main() {
  try {
    verify_merge_renumbers_and_rewrites();
    verify_merge_is_additive_on_replay();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "document-merge-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
