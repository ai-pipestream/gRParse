#include "grparse/office_fold/arena.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

#include "grparse/office_fold/grid_cells.h"
#include "grparse/office_fold/value_convert.h"
#include "grparse/schema_version.h"

namespace grparse::office_fold {

namespace {

// The schema identity carried on the document root: the wire schema name
// and the schema minor this repo currently mirrors. Every producer in the
// fleet stamps the same pair.
constexpr const char* kSchemaName = kWireSchemaName;
constexpr const char* kSchemaVersion = kUpstreamSchemaVersion;

// The index a "#/<arena>/N" reference names, or -1 when the reference does
// not name that arena.
int index_in_arena(const std::string& ref, const std::string& prefix) {
  if (!ref.starts_with(prefix)) return -1;
  return std::atoi(ref.c_str() + prefix.size());
}

}  // namespace

DocumentArena::DocumentArena() {
  document_.set_schema_name(kSchemaName);
  document_.set_version(kSchemaVersion);
  docv1::GroupItem* body = document_.mutable_body();
  body->set_self_ref("#/body");
  body->set_content_layer(docv1::CONTENT_LAYER_BODY);
  docv1::GroupItem* furniture = document_.mutable_furniture();
  furniture->set_self_ref("#/furniture");
  furniture->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
}

void DocumentArena::set_page_rects(
    const google::protobuf::RepeatedPtrField<officev1::PageRect>& rects) {
  page_rects_.assign(rects.begin(), rects.end());
}

int DocumentArena::page_for_point(double x, double y) const {
  for (int index = 0; index < static_cast<int>(page_rects_.size()); index++) {
    const officev1::PageRect& page = page_rects_[index];
    if (x >= static_cast<double>(page.x_twips())
        && x < static_cast<double>(page.x_twips() + page.width_twips())
        && y >= static_cast<double>(page.y_twips())
        && y < static_cast<double>(page.y_twips() + page.height_twips())) {
      return index;
    }
  }
  return -1;
}

docv1::GroupItem* DocumentArena::group_by_ref(const std::string& ref) {
  if (ref == "#/body") return document_.mutable_body();
  if (ref == "#/furniture") return document_.mutable_furniture();
  const int index = index_in_arena(ref, "#/groups/");
  if (index >= 0 && index < document_.groups_size()) {
    return document_.mutable_groups(index);
  }
  return document_.mutable_body();
}

bool DocumentArena::link_into_item_arena(const std::string& parent_ref,
                                         const std::string& child_ref) {
  // group_by_ref falls back to the body, so the arenas that own children of
  // their own are matched here first; otherwise a field's children would
  // silently land in the body instead of under their field. A picture owns
  // its caption and, for a chart, its data table; a table owns its caption.
  if (int index = index_in_arena(parent_ref, "#/field_regions/");
      index >= 0 && index < document_.field_regions_size()) {
    document_.mutable_field_regions(index)->add_children()->set_ref(child_ref);
    return true;
  }
  if (int index = index_in_arena(parent_ref, "#/field_items/");
      index >= 0 && index < document_.field_items_size()) {
    document_.mutable_field_items(index)->add_children()->set_ref(child_ref);
    return true;
  }
  if (int index = index_in_arena(parent_ref, "#/pictures/");
      index >= 0 && index < document_.pictures_size()) {
    document_.mutable_pictures(index)->add_children()->set_ref(child_ref);
    return true;
  }
  if (int index = index_in_arena(parent_ref, "#/tables/");
      index >= 0 && index < document_.tables_size()) {
    document_.mutable_tables(index)->add_children()->set_ref(child_ref);
    return true;
  }
  return false;
}

void DocumentArena::link_child(const std::string& parent_ref,
                               const std::string& child_ref) {
  if (link_into_item_arena(parent_ref, child_ref)) return;
  group_by_ref(parent_ref)->add_children()->set_ref(child_ref);
}

void DocumentArena::stamp_collector_source(SourceTypes* source) {
  // Every item this mapper creates is attributable: additive merges with
  // other collectors' output rely on the tag to never collide silently.
  docv1::CollectorSource* collector = source->Add()->mutable_collector();
  collector->set_collector("libreoffice");
  collector->set_model("lok");
}

docv1::GroupItem* DocumentArena::add_group(const std::string& parent_ref,
                                           docv1::GroupLabel label,
                                           const std::string& name,
                                           docv1::ContentLayer layer) {
  int index = document_.groups_size();
  docv1::GroupItem* group = document_.add_groups();
  group->set_self_ref("#/groups/" + std::to_string(index));
  group->mutable_parent()->set_ref(parent_ref);
  group->set_label(label);
  group->set_content_layer(layer);
  if (!name.empty()) group->set_name(name);
  link_child(parent_ref, group->self_ref());
  return group;
}

TextHandle DocumentArena::add_text(TextKind kind, docv1::DocItemLabel label,
                                   docv1::ContentLayer layer,
                                   const std::string& parent_ref) {
  TextHandle handle;
  handle.ref = "#/texts/" + std::to_string(document_.texts_size());
  handle.item = document_.add_texts();
  switch (kind) {
    case TextKind::kTitle:
      handle.base = handle.item->mutable_title()->mutable_base();
      break;
    case TextKind::kSectionHeader:
      handle.base = handle.item->mutable_section_header()->mutable_base();
      break;
    case TextKind::kList:
      handle.base = handle.item->mutable_list_item()->mutable_base();
      break;
    case TextKind::kFormula:
      handle.base = handle.item->mutable_formula()->mutable_base();
      break;
    case TextKind::kText:
      handle.base = handle.item->mutable_text()->mutable_base();
      break;
    case TextKind::kFieldHeading:
      handle.base = handle.item->mutable_field_heading()->mutable_base();
      break;
    case TextKind::kFieldValue:
      handle.base = handle.item->mutable_field_value()->mutable_base();
      break;
  }
  handle.base->set_self_ref(handle.ref);
  handle.base->mutable_parent()->set_ref(parent_ref);
  handle.base->set_label(label);
  handle.base->set_content_layer(layer);
  stamp_collector_source(handle.base->mutable_source());
  link_child(parent_ref, handle.ref);
  return handle;
}

docv1::PictureItem* DocumentArena::add_picture(docv1::DocItemLabel label,
                                               docv1::ContentLayer layer,
                                               const std::string& parent_ref,
                                               std::string* ref_out) {
  std::string ref = "#/pictures/" + std::to_string(document_.pictures_size());
  docv1::PictureItem* picture = document_.add_pictures();
  picture->set_self_ref(ref);
  picture->mutable_parent()->set_ref(parent_ref);
  picture->set_label(label);
  picture->set_content_layer(layer);
  stamp_collector_source(picture->mutable_source());
  link_child(parent_ref, ref);
  if (ref_out != nullptr) *ref_out = ref;
  return picture;
}

docv1::TableItem* DocumentArena::add_table(docv1::ContentLayer layer,
                                           const std::string& parent_ref,
                                           std::string* ref_out) {
  std::string ref = "#/tables/" + std::to_string(document_.tables_size());
  docv1::TableItem* table = document_.add_tables();
  table->set_self_ref(ref);
  table->mutable_parent()->set_ref(parent_ref);
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  table->set_content_layer(layer);
  stamp_collector_source(table->mutable_source());
  link_child(parent_ref, ref);
  if (ref_out != nullptr) *ref_out = ref;
  return table;
}

void DocumentArena::move_child_after(const std::string& parent_ref,
                                     const std::string& child_ref,
                                     const std::string& after_ref) {
  ChildRefs* children = group_by_ref(parent_ref)->mutable_children();
  int from = -1;
  int anchor = -1;
  for (int i = 0; i < children->size(); i++) {
    if (children->Get(i).ref() == child_ref) from = i;
    if (!after_ref.empty() && children->Get(i).ref() == after_ref) anchor = i;
  }
  if (from < 0) return;
  const int to = anchor < from ? anchor + 1 : anchor;
  if (to == from) return;
  // Rotate the child into place; every reference stays what it was.
  if (to < from) {
    for (int i = from; i > to; i--) children->SwapElements(i, i - 1);
  } else {
    for (int i = from; i < to; i++) children->SwapElements(i, i + 1);
  }
}

docv1::TextItemBase* DocumentArena::text_by_ref(const std::string& ref) {
  const int index = index_in_arena(ref, "#/texts/");
  if (index < 0 || index >= document_.texts_size()) return nullptr;
  docv1::BaseTextItem* item = document_.mutable_texts(index);
  switch (item->item_case()) {
    case docv1::BaseTextItem::kTitle:
      return item->mutable_title()->mutable_base();
    case docv1::BaseTextItem::kSectionHeader:
      return item->mutable_section_header()->mutable_base();
    case docv1::BaseTextItem::kListItem:
      return item->mutable_list_item()->mutable_base();
    case docv1::BaseTextItem::kFormula:
      return item->mutable_formula()->mutable_base();
    case docv1::BaseTextItem::kText:
      return item->mutable_text()->mutable_base();
    case docv1::BaseTextItem::kFieldHeading:
      return item->mutable_field_heading()->mutable_base();
    case docv1::BaseTextItem::kFieldValue:
      return item->mutable_field_value()->mutable_base();
    case docv1::BaseTextItem::kCode:
    case docv1::BaseTextItem::ITEM_NOT_SET:
      return nullptr;
  }
  return nullptr;
}

void DocumentArena::to_page_local(int page_index, double* l, double* t,
                                  double* r, double* b) {
  if (page_index < static_cast<int>(page_rects_.size())) {
    const officev1::PageRect& page = page_rects_[page_index];
    *l -= static_cast<double>(page.x_twips());
    *r -= static_cast<double>(page.x_twips());
    *t -= static_cast<double>(page.y_twips());
    *b -= static_cast<double>(page.y_twips());
    return;
  }
  if (!unresolved_prov_pages_.insert(page_index).second) return;
  // No page rectangle to subtract, so the box stays document-absolute
  // while every emitted box claims to be page-local. The item keeps its
  // provenance (dropping it loses the page number too), but the fold
  // says so once per page rather than letting the consumer trust a
  // coordinate space that does not hold.
  warn("page " + std::to_string(page_index + 1)
       + " has no known rectangle, so its provenance boxes stay "
         "document-absolute despite the page-local coordinate origin");
}

void DocumentArena::add_prov(ProvenanceItems* prov, int page_index,
                             bool page_local, double l, double t, double r,
                             double b, long long span_start,
                             long long span_end) {
  if (page_index < 0) return;
  if (!page_local) to_page_local(page_index, &l, &t, &r, &b);
  docv1::ProvenanceItem* item = prov->Add();
  item->set_page_no(page_index + 1);
  docv1::BoundingBox* box = item->mutable_bbox();
  box->set_l(l);
  box->set_t(t);
  box->set_r(r);
  box->set_b(b);
  box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  item->mutable_charspan()->set_start(clamp32(span_start));
  item->mutable_charspan()->set_end(clamp32(span_end));
}

void DocumentArena::add_line_prov(ProvenanceItems* prov,
                                  const LineBoxes& lines, long long span_start,
                                  long long span_end) {
  for (const officev1::LineBox& line : lines) {
    // A line carrying measured character boundaries narrows its charspan to
    // its own characters, offset into the item's span space; unmeasured
    // lines keep the full item span. char_end must exceed char_start so the
    // -1 sentinel and a defaulted [0, 0) both fall back.
    long long start = span_start;
    long long end = span_end;
    if (line.char_start() >= 0 && line.char_end() > line.char_start()) {
      start = span_start + line.char_start();
      end = span_start + line.char_end();
      if (span_end > span_start) {
        start = std::min(start, span_end);
        end = std::min(end, span_end);
      }
    }
    add_prov(prov, line.page_index(), false,
             static_cast<double>(line.x_twips()),
             static_cast<double>(line.y_twips()),
             static_cast<double>(line.x_twips() + line.width_twips()),
             static_cast<double>(line.y_twips() + line.height_twips()),
             start, end);
  }
}

void DocumentArena::add_caret_prov(ProvenanceItems* prov, int page_index,
                                   const officev1::TwipsPoint& start,
                                   const officev1::TwipsPoint& end,
                                   long long span_start, long long span_end) {
  add_prov(prov, page_index, false,
           static_cast<double>(std::min(start.x(), end.x())),
           static_cast<double>(std::min(start.y(), end.y())),
           static_cast<double>(std::max(start.x(), end.x())),
           static_cast<double>(std::max(start.y(), end.y())),
           span_start, span_end);
}

bool DocumentArena::cell_bbox(const LineBoxes& lines,
                              docv1::BoundingBox* box) {
  // The union covers only the lines on the cell's first page: TableCell has
  // no page slot, so a cell straddling a page break keeps its first page's
  // extent.
  int page_index = -1;
  double l = 0, t = 0, r = 0, b = 0;
  bool first = true;
  for (const officev1::LineBox& line : lines) {
    if (line.page_index() < 0) continue;
    if (page_index < 0) page_index = line.page_index();
    if (line.page_index() != page_index) continue;
    double ll = static_cast<double>(line.x_twips());
    double lt = static_cast<double>(line.y_twips());
    double lr = ll + static_cast<double>(line.width_twips());
    double lb = lt + static_cast<double>(line.height_twips());
    if (first) {
      l = ll; t = lt; r = lr; b = lb;
      first = false;
    } else {
      l = std::min(l, ll);
      t = std::min(t, lt);
      r = std::max(r, lr);
      b = std::max(b, lb);
    }
  }
  if (first) return false;
  // Line rectangles are document-absolute like every LineBox; page-local
  // like add_prov, but silently: a cell has no page slot to be wrong about.
  if (page_index < static_cast<int>(page_rects_.size())) {
    const officev1::PageRect& page = page_rects_[page_index];
    l -= static_cast<double>(page.x_twips());
    r -= static_cast<double>(page.x_twips());
    t -= static_cast<double>(page.y_twips());
    b -= static_cast<double>(page.y_twips());
  }
  box->set_l(l);
  box->set_t(t);
  box->set_r(r);
  box->set_b(b);
  box->set_coord_origin(docv1::COORD_ORIGIN_TOPLEFT);
  return true;
}

void DocumentArena::fold_table(const officev1::TableData& table,
                               docv1::TableItem* item) {
  docv1::TableData* data = item->mutable_data();
  data->set_num_rows(table.rows());
  data->set_num_cols(table.columns());
  for (const officev1::TableCellData& cell : table.cells()) {
    // A split or merged office cell has no base-grid position of its own,
    // but its name anchors at one; placing it there with its merge spans is
    // what makes a merged table structurally readable. Only a name that
    // anchors nowhere has nowhere to go.
    int row = cell.row();
    int column = cell.column();
    if ((row < 0 || column < 0)
        && !anchor_of_cell_name(cell.name(), &row, &column)) {
      row = -1;
      column = -1;
    }
    if (row < 0 || column < 0) {
      (*item->mutable_meta()->mutable_custom_fields())["cell:" + cell.name()] =
          str_value(cell.text());
      continue;
    }
    int row_span = std::max(1, cell.row_span());
    int col_span = std::max(1, cell.column_span());
    docv1::TableCell* out = data->add_table_cells();
    out->set_start_row_offset_idx(row);
    out->set_end_row_offset_idx(row + row_span);
    out->set_start_col_offset_idx(column);
    out->set_end_col_offset_idx(column + col_span);
    out->set_row_span(row_span);
    out->set_col_span(col_span);
    out->set_text(cell.text());
    if (!cell.line_rects().empty()) {
      docv1::BoundingBox box;
      if (cell_bbox(cell.line_rects(), &box)) *out->mutable_bbox() = box;
    }
  }
  fill_grid_from_cells(data);
}

}  // namespace grparse::office_fold
