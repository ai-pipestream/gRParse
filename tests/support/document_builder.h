// Hand-built ai.pipestream.document.v1.Document fixtures: the arena writes
// every renderer, geometry, and normalization test needs, spelled once.
// Header only, and test-only: nothing under src/ or include/ may include it.
//
// The builders keep the invariants a merged document always has, because the
// units under test read them: an item lives in its arena, names its own
// self_ref, names its parent, and is linked from that parent's children.
#ifndef GRPARSE_TESTS_SUPPORT_DOCUMENT_BUILDER_H
#define GRPARSE_TESTS_SUPPORT_DOCUMENT_BUILDER_H

#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "support/check.h"

namespace grparse_test {

namespace docv1 = ai::pipestream::document::v1;

// An empty document with both roots in place, which is the state a collector
// starts from and the smallest thing every renderer accepts.
inline docv1::Document base_document(const std::string& name) {
  docv1::Document document;
  document.set_name(name);
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  return document;
}

// Links a child reference under its parent: a root or a group.
inline void attach(docv1::Document* document, const std::string& parent,
                   const std::string& child) {
  if (parent == "#/body") {
    document->mutable_body()->add_children()->set_ref(child);
    return;
  }
  if (parent == "#/furniture") {
    document->mutable_furniture()->add_children()->set_ref(child);
    return;
  }
  const std::string prefix = "#/groups/";
  require(parent.starts_with(prefix), "fixture parent must be a root or a group");
  document->mutable_groups(std::stoi(parent.substr(prefix.size())))
      ->add_children()
      ->set_ref(child);
}

// Appends one text arena entry of the requested variant and links it under
// its parent.  Returns the new item's reference.
inline std::string add_text(docv1::Document* document, const std::string& parent,
                            docv1::BaseTextItem::ItemCase variant,
                            docv1::DocItemLabel label, const std::string& text,
                            int level = 0, bool enumerated = false) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* item = document->add_texts();
  docv1::TextItemBase* base = nullptr;
  switch (variant) {
    case docv1::BaseTextItem::kTitle:
      base = item->mutable_title()->mutable_base();
      break;
    case docv1::BaseTextItem::kSectionHeader: {
      auto* header = item->mutable_section_header();
      header->set_level(level);
      base = header->mutable_base();
      break;
    }
    case docv1::BaseTextItem::kListItem: {
      auto* list_item = item->mutable_list_item();
      list_item->set_enumerated(enumerated);
      base = list_item->mutable_base();
      break;
    }
    case docv1::BaseTextItem::kFormula:
      base = item->mutable_formula()->mutable_base();
      break;
    default:
      base = item->mutable_text()->mutable_base();
      break;
  }
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref(parent);
  base->set_label(label);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  attach(document, parent, ref);
  return ref;
}

// A paragraph, the shape most body text takes.
inline std::string add_paragraph(docv1::Document* document, const std::string& parent,
                                 const std::string& text) {
  return add_text(document, parent, docv1::BaseTextItem::kText,
                  docv1::DOC_ITEM_LABEL_TEXT, text);
}

inline std::string add_heading(docv1::Document* document, const std::string& parent,
                               const std::string& text, int level) {
  return add_text(document, parent, docv1::BaseTextItem::kSectionHeader,
                  docv1::DOC_ITEM_LABEL_SECTION_HEADER, text, level);
}

// CodeItem inlines the base fields instead of nesting them, so it gets its
// own writer.
inline std::string add_code(docv1::Document* document, const std::string& parent,
                            const std::string& text, docv1::CodeLanguageLabel language) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* code = document->add_texts()->mutable_code();
  code->set_self_ref(ref);
  code->mutable_parent()->set_ref(parent);
  code->set_label(docv1::DOC_ITEM_LABEL_CODE);
  code->set_content_layer(docv1::CONTENT_LAYER_BODY);
  code->set_text(text);
  code->set_code_language(language);
  attach(document, parent, ref);
  return ref;
}

inline std::string add_group(docv1::Document* document, const std::string& parent,
                             docv1::GroupLabel label) {
  const std::string ref = "#/groups/" + std::to_string(document->groups_size());
  auto* group = document->add_groups();
  group->set_self_ref(ref);
  group->mutable_parent()->set_ref(parent);
  group->set_label(label);
  group->set_content_layer(docv1::CONTENT_LAYER_BODY);
  attach(document, parent, ref);
  return ref;
}

// A caption or footnote text lives in the text arena with the owning table or
// figure as its parent; it is reached from the owner's list, never linked
// under the body.
inline std::string add_owned_text(docv1::Document* document, const std::string& owner,
                                  docv1::DocItemLabel label, const std::string& text) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* base = document->add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref(owner);
  base->set_label(label);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(text);
  return ref;
}

// A rich table cell's block group: it lives in the group arena with the
// owning table as its parent and is reached only through the cell's ref,
// never linked under the body. Children attach through the returned ref.
inline std::string add_owned_group(docv1::Document* document, const std::string& owner,
                                   docv1::GroupLabel label) {
  const std::string ref = "#/groups/" + std::to_string(document->groups_size());
  auto* group = document->add_groups();
  group->set_self_ref(ref);
  group->mutable_parent()->set_ref(owner);
  group->set_label(label);
  group->set_content_layer(docv1::CONTENT_LAYER_BODY);
  return ref;
}

inline docv1::TableItem* add_table(docv1::Document* document, const std::string& parent) {
  const std::string ref = "#/tables/" + std::to_string(document->tables_size());
  auto* table = document->add_tables();
  table->set_self_ref(ref);
  table->mutable_parent()->set_ref(parent);
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  table->set_content_layer(docv1::CONTENT_LAYER_BODY);
  attach(document, parent, ref);
  return table;
}

// Writes one cell into both projections a producer fills: the flat cell list
// the exports read and the wire grid the markup renderers read.  The returned
// pointer is the flat-list entry, so a field set afterwards reaches the list
// the grid is derived from; a case that wants the wire grid to carry it must
// write the row copy itself.
inline docv1::TableCell* add_cell(docv1::TableData* data, docv1::TableRow* row,
                                  const std::string& text, bool column_header,
                                  int row_index, int col_index, int row_span = 1,
                                  int col_span = 1) {
  auto* cell = data->add_table_cells();
  cell->set_text(text);
  cell->set_column_header(column_header);
  cell->set_row_span(row_span);
  cell->set_col_span(col_span);
  cell->set_start_row_offset_idx(row_index);
  cell->set_end_row_offset_idx(row_index + row_span);
  cell->set_start_col_offset_idx(col_index);
  cell->set_end_col_offset_idx(col_index + col_span);
  if (row != nullptr) *row->add_cells() = *cell;
  return cell;
}

inline docv1::PictureItem* add_picture(docv1::Document* document, const std::string& parent,
                                       const std::string& uri) {
  const std::string ref = "#/pictures/" + std::to_string(document->pictures_size());
  auto* picture = document->add_pictures();
  picture->set_self_ref(ref);
  picture->mutable_parent()->set_ref(parent);
  picture->set_label(docv1::DOC_ITEM_LABEL_PICTURE);
  picture->set_content_layer(docv1::CONTENT_LAYER_BODY);
  if (!uri.empty()) picture->mutable_image()->set_uri(uri);
  attach(document, parent, ref);
  return picture;
}

inline docv1::PageItem* add_page(docv1::Document* document, int page_no, double width,
                                 double height) {
  auto& page = (*document->mutable_pages())[page_no];
  page.set_page_no(page_no);
  page.mutable_size()->set_width(width);
  page.mutable_size()->set_height(height);
  return &page;
}

// One provenance entry, top-left origin unless the caller says otherwise.
inline docv1::ProvenanceItem* add_prov(
    google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* prov, int page_no, double left,
    double top, double right, double bottom,
    docv1::CoordOrigin origin = docv1::COORD_ORIGIN_TOPLEFT) {
  auto* entry = prov->Add();
  entry->set_page_no(page_no);
  auto* box = entry->mutable_bbox();
  box->set_l(left);
  box->set_t(top);
  box->set_r(right);
  box->set_b(bottom);
  box->set_coord_origin(origin);
  return entry;
}

// Attributes an item to a collector, which is what produced_only_by reads.
inline void add_collector_source(google::protobuf::RepeatedPtrField<docv1::SourceType>* sources,
                                 const std::string& collector) {
  sources->Add()->mutable_collector()->set_collector(collector);
}

}  // namespace grparse_test

#endif
