// Ported from grpc-libreoffice src/docling_map.cpp (the canonical copy).
// Keep in sync until the protos-home work gives the mapper one home.
#include "grparse/docling_map.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <sstream>

#include <google/protobuf/struct.pb.h>

namespace grparse {

namespace docv1 = ai::pipestream::document::v1;
namespace officev1 = ai::pipestream::office::v1;

namespace {

// The schema identity docling-core v2 interop expects on the root.
constexpr const char* kSchemaName = "docling_document_v2";

// Grids above this cell count keep table_cells only; a fully materialized
// grid over a sparse used range would dwarf the data it carries.
constexpr int kMaxGridCells = 4096;

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size()
      && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int32_t clamp32(long long value) {
  if (value < 0) return 0;
  if (value > INT32_MAX) return INT32_MAX;
  return static_cast<int32_t>(value);
}

std::string base64(const std::string& data) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  while (i + 2 < data.size()) {
    uint32_t chunk = (static_cast<unsigned char>(data[i]) << 16)
        | (static_cast<unsigned char>(data[i + 1]) << 8)
        | static_cast<unsigned char>(data[i + 2]);
    out.push_back(alphabet[(chunk >> 18) & 63]);
    out.push_back(alphabet[(chunk >> 12) & 63]);
    out.push_back(alphabet[(chunk >> 6) & 63]);
    out.push_back(alphabet[chunk & 63]);
    i += 3;
  }
  if (i + 1 == data.size()) {
    uint32_t chunk = static_cast<unsigned char>(data[i]) << 16;
    out.push_back(alphabet[(chunk >> 18) & 63]);
    out.push_back(alphabet[(chunk >> 12) & 63]);
    out.append("==");
  } else if (i + 2 == data.size()) {
    uint32_t chunk = (static_cast<unsigned char>(data[i]) << 16)
        | (static_cast<unsigned char>(data[i + 1]) << 8);
    out.push_back(alphabet[(chunk >> 18) & 63]);
    out.push_back(alphabet[(chunk >> 12) & 63]);
    out.push_back(alphabet[(chunk >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::string data_uri(const std::string& mime, const std::string& bytes) {
  return "data:" + mime + ";base64," + base64(bytes);
}

std::string mime_for_extension(const std::string& extension) {
  static const std::map<std::string, std::string> kMimes = {
      {"doc", "application/msword"},
      {"docx", "application/vnd.openxmlformats-officedocument"
               ".wordprocessingml.document"},
      {"xls", "application/vnd.ms-excel"},
      {"xlsx", "application/vnd.openxmlformats-officedocument"
               ".spreadsheetml.sheet"},
      {"ppt", "application/vnd.ms-powerpoint"},
      {"pptx", "application/vnd.openxmlformats-officedocument"
               ".presentationml.presentation"},
      {"odt", "application/vnd.oasis.opendocument.text"},
      {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
      {"odp", "application/vnd.oasis.opendocument.presentation"},
      {"odg", "application/vnd.oasis.opendocument.graphics"},
      {"fodt", "application/vnd.oasis.opendocument.text"},
      {"fods", "application/vnd.oasis.opendocument.spreadsheet"},
      {"fodp", "application/vnd.oasis.opendocument.presentation"},
      {"fodg", "application/vnd.oasis.opendocument.graphics"},
      {"rtf", "application/rtf"},
      {"csv", "text/csv"},
      {"txt", "text/plain"},
      {"html", "text/html"},
      {"pdf", "application/pdf"},
  };
  auto found = kMimes.find(extension);
  return found != kMimes.end() ? found->second : "application/octet-stream";
}

google::protobuf::Value str_value(const std::string& text) {
  google::protobuf::Value value;
  value.set_string_value(text);
  return value;
}

google::protobuf::Value num_value(double number) {
  google::protobuf::Value value;
  value.set_number_value(number);
  return value;
}

google::protobuf::Value bool_value(bool flag) {
  google::protobuf::Value value;
  value.set_bool_value(flag);
  return value;
}

std::string concat_runs(
    const google::protobuf::RepeatedPtrField<officev1::TextRun>& runs) {
  std::string text;
  for (const officev1::TextRun& run : runs) text += run.text();
  return text;
}

long long runs_length(
    const google::protobuf::RepeatedPtrField<officev1::TextRun>& runs) {
  long long total = 0;
  for (const officev1::TextRun& run : runs) total += run.char_length();
  return total;
}

// Sets item-level Formatting when every run agrees on the four flags docling
// models; mixed-format text keeps formatting unset.
void set_uniform_formatting(
    const google::protobuf::RepeatedPtrField<officev1::TextRun>& runs,
    docv1::TextItemBase* base) {
  if (runs.empty()) return;
  bool bold = runs[0].weight() >= 150.0f;
  for (const officev1::TextRun& run : runs) {
    if ((run.weight() >= 150.0f) != bold || run.italic() != runs[0].italic()
        || run.underline() != runs[0].underline()
        || run.strikethrough() != runs[0].strikethrough()) {
      return;
    }
  }
  if (!bold && !runs[0].italic() && !runs[0].underline()
      && !runs[0].strikethrough()) {
    return;
  }
  docv1::Formatting* formatting = base->mutable_formatting();
  formatting->set_bold(bold);
  formatting->set_italic(runs[0].italic());
  formatting->set_underline(runs[0].underline());
  formatting->set_strikethrough(runs[0].strikethrough());
}

// Attaches the runs' hyperlinks to the item: the first link lands in the
// docling hyperlink slot, and every link gets an entry in the "hyperlinks"
// custom field with its item-local character span, merging the runs a link
// was split into by other formatting boundaries.
void apply_run_hyperlinks(
    const google::protobuf::RepeatedPtrField<officev1::TextRun>& runs,
    docv1::TextItemBase* base) {
  google::protobuf::ListValue links;
  long long local = 0;
  std::string url;
  std::string target;
  std::string name;
  long long start = 0;
  auto flush = [&]() {
    if (url.empty()) return;
    google::protobuf::Struct* link =
        links.add_values()->mutable_struct_value();
    (*link->mutable_fields())["url"] = str_value(url);
    if (!target.empty()) (*link->mutable_fields())["target"] = str_value(target);
    if (!name.empty()) (*link->mutable_fields())["name"] = str_value(name);
    (*link->mutable_fields())["char_start"] =
        num_value(static_cast<double>(start));
    (*link->mutable_fields())["char_end"] =
        num_value(static_cast<double>(local));
    url.clear();
    target.clear();
    name.clear();
  };
  for (const officev1::TextRun& run : runs) {
    if (run.hyperlink_url() != url || run.hyperlink_target() != target ||
        run.hyperlink_name() != name) {
      flush();
      url = run.hyperlink_url();
      target = run.hyperlink_target();
      name = run.hyperlink_name();
      start = local;
    }
    local += run.char_length();
  }
  flush();
  if (links.values_size() == 0) return;
  base->set_hyperlink(
      links.values(0).struct_value().fields().at("url").string_value());
  google::protobuf::Value value;
  *value.mutable_list_value() = std::move(links);
  (*base->mutable_meta()->mutable_custom_fields())["hyperlinks"] =
      std::move(value);
}

std::string column_name(int column) {
  std::string name;
  for (int c = column; c >= 0; c = c / 26 - 1) {
    name.insert(name.begin(), static_cast<char>('A' + c % 26));
  }
  return name;
}

std::string a1_name(int row, int column) {
  return column_name(column) + std::to_string(row + 1);
}

std::string range_a1(const officev1::SheetRangeRef& range) {
  return a1_name(range.start_row(), range.start_column()) + ":"
      + a1_name(range.end_row(), range.end_column());
}

}  // namespace

DoclingMapper::DoclingMapper() {
  document_.set_schema_name(kSchemaName);
  docv1::GroupItem* body = document_.mutable_body();
  body->set_self_ref("#/body");
  body->set_content_layer(docv1::CONTENT_LAYER_BODY);
  docv1::GroupItem* furniture = document_.mutable_furniture();
  furniture->set_self_ref("#/furniture");
  furniture->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
}

docv1::GroupItem* DoclingMapper::group_by_ref(const std::string& ref) {
  if (ref == "#/body") return document_.mutable_body();
  if (ref == "#/furniture") return document_.mutable_furniture();
  const std::string prefix = "#/groups/";
  if (ref.compare(0, prefix.size(), prefix) == 0) {
    int index = std::atoi(ref.c_str() + prefix.size());
    if (index >= 0 && index < document_.groups_size()) {
      return document_.mutable_groups(index);
    }
  }
  return document_.mutable_body();
}

void DoclingMapper::link_child(const std::string& parent_ref,
                               const std::string& child_ref) {
  group_by_ref(parent_ref)->add_children()->set_ref(child_ref);
}

void DoclingMapper::stamp_collector_source(
    google::protobuf::RepeatedPtrField<docv1::SourceType>* source) {
  // Every item this mapper creates is attributable: additive merges with
  // other collectors' output rely on the tag to never collide silently.
  docv1::CollectorSource* collector = source->Add()->mutable_collector();
  collector->set_collector("libreoffice");
  collector->set_model("lok");
}

docv1::GroupItem* DoclingMapper::add_group(const std::string& parent_ref,
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

DoclingMapper::TextHandle DoclingMapper::add_text(TextKind kind,
                                                  docv1::DocItemLabel label,
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
  }
  handle.base->set_self_ref(handle.ref);
  handle.base->mutable_parent()->set_ref(parent_ref);
  handle.base->set_label(label);
  handle.base->set_content_layer(layer);
  stamp_collector_source(handle.base->mutable_source());
  link_child(parent_ref, handle.ref);
  return handle;
}

docv1::PictureItem* DoclingMapper::add_picture(docv1::DocItemLabel label,
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

docv1::TableItem* DoclingMapper::add_table(docv1::ContentLayer layer,
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

void DoclingMapper::add_prov(
    google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* prov,
    int page_index, bool page_local, double l, double t, double r, double b,
    long long span_start, long long span_end) {
  if (page_index < 0) return;
  if (!page_local && page_index < static_cast<int>(page_rects_.size())) {
    const officev1::PageRect& page = page_rects_[page_index];
    l -= static_cast<double>(page.x_twips());
    r -= static_cast<double>(page.x_twips());
    t -= static_cast<double>(page.y_twips());
    b -= static_cast<double>(page.y_twips());
  }
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

void DoclingMapper::add_line_prov(
    google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* prov,
    const google::protobuf::RepeatedPtrField<officev1::LineBox>& lines,
    long long span_start, long long span_end) {
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

void DoclingMapper::add_caret_prov(
    google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* prov,
    int page_index, const officev1::TwipsPoint& start,
    const officev1::TwipsPoint& end, long long span_start,
    long long span_end) {
  add_prov(prov, page_index, false,
           static_cast<double>(std::min(start.x(), end.x())),
           static_cast<double>(std::min(start.y(), end.y())),
           static_cast<double>(std::max(start.x(), end.x())),
           static_cast<double>(std::max(start.y(), end.y())),
           span_start, span_end);
}

bool DoclingMapper::cell_bbox(
    const google::protobuf::RepeatedPtrField<officev1::LineBox>& lines,
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
  // like add_prov.
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

void DoclingMapper::fold_table(const officev1::TableData& table,
                               docv1::TableItem* item) {
  docv1::TableData* data = item->mutable_data();
  data->set_num_rows(table.rows());
  data->set_num_cols(table.columns());
  for (const officev1::TableCellData& cell : table.cells()) {
    if (cell.row() < 0 || cell.column() < 0) {
      // Split or merged office cells fall outside the base grid; their text
      // stays addressable by office cell name.
      (*item->mutable_meta()->mutable_custom_fields())["cell:" + cell.name()] =
          str_value(cell.text());
      continue;
    }
    docv1::TableCell* out = data->add_table_cells();
    out->set_start_row_offset_idx(cell.row());
    out->set_end_row_offset_idx(cell.row() + 1);
    out->set_start_col_offset_idx(cell.column());
    out->set_end_col_offset_idx(cell.column() + 1);
    out->set_row_span(1);
    out->set_col_span(1);
    out->set_text(cell.text());
    if (!cell.line_rects().empty()) {
      docv1::BoundingBox box;
      if (cell_bbox(cell.line_rects(), &box)) *out->mutable_bbox() = box;
    }
  }
  if (table.rows() > 0 && table.columns() > 0
      && table.rows() * table.columns() <= kMaxGridCells) {
    for (int row = 0; row < table.rows(); row++) {
      docv1::TableRow* out_row = data->add_grid();
      for (int column = 0; column < table.columns(); column++) {
        docv1::TableCell* out = out_row->add_cells();
        out->set_start_row_offset_idx(row);
        out->set_end_row_offset_idx(row + 1);
        out->set_start_col_offset_idx(column);
        out->set_end_col_offset_idx(column + 1);
        out->set_row_span(1);
        out->set_col_span(1);
      }
    }
    for (const docv1::TableCell& cell : data->table_cells()) {
      docv1::TableCell* slot = data->mutable_grid(cell.start_row_offset_idx())
          ->mutable_cells(cell.start_col_offset_idx());
      slot->set_text(cell.text());
      if (cell.has_bbox()) *slot->mutable_bbox() = cell.bbox();
    }
  }
}

void DoclingMapper::consume(const officev1::StreamPagesResponse& event) {
  switch (event.event_case()) {
    case officev1::StreamPagesResponse::kDocumentInfo:
      return on_document_info(event.document_info());
    case officev1::StreamPagesResponse::kPageImage:
      return on_page_image(event.page_image());
    case officev1::StreamPagesResponse::kStatus:
      return on_status(event.status());
    case officev1::StreamPagesResponse::kMetadata:
      return on_metadata(event.metadata());
    case officev1::StreamPagesResponse::kParagraph:
      return on_paragraph(event.paragraph());
    case officev1::StreamPagesResponse::kTable:
      return on_table(event.table());
    case officev1::StreamPagesResponse::kEmbeddedImage:
      return on_embedded_image(event.embedded_image());
    case officev1::StreamPagesResponse::kFootnote:
      return on_footnote(event.footnote());
    case officev1::StreamPagesResponse::kHeaderFooter:
      return on_header_footer(event.header_footer());
    case officev1::StreamPagesResponse::kPageStyle:
      return on_page_style(event.page_style());
    case officev1::StreamPagesResponse::kDocumentIndex:
      return on_document_index(event.document_index());
    case officev1::StreamPagesResponse::kDrawingShape:
      return on_drawing_shape(event.drawing_shape());
    case officev1::StreamPagesResponse::kSlide:
      return on_slide(event.slide());
    case officev1::StreamPagesResponse::kSlideShape:
      return on_slide_shape(event.slide_shape());
    case officev1::StreamPagesResponse::kTextFrame:
      return on_text_frame(event.text_frame());
    case officev1::StreamPagesResponse::kShape:
      return on_shape(event.shape());
    case officev1::StreamPagesResponse::kEmbeddedObject:
      return on_embedded_object(event.embedded_object());
    case officev1::StreamPagesResponse::kSheet:
      return on_sheet(event.sheet());
    case officev1::StreamPagesResponse::kSheetRow:
      return on_sheet_row(event.sheet_row());
    case officev1::StreamPagesResponse::kSheetNamedRange:
      return on_sheet_named_range(event.sheet_named_range());
    case officev1::StreamPagesResponse::kSheetDatabaseRange:
      return on_sheet_database_range(event.sheet_database_range());
    case officev1::StreamPagesResponse::kSheetCellComment:
      return on_sheet_cell_comment(event.sheet_cell_comment());
    case officev1::StreamPagesResponse::kSheetChart:
      return on_sheet_chart(event.sheet_chart());
    case officev1::StreamPagesResponse::kSheetPivotTable:
      return on_sheet_pivot_table(event.sheet_pivot_table());
    case officev1::StreamPagesResponse::kComment:
      return on_comment(event.comment());
    case officev1::StreamPagesResponse::kTrackedChange:
      return on_tracked_change(event.tracked_change());
    case officev1::StreamPagesResponse::kBookmark:
      return on_bookmark(event.bookmark());
    case officev1::StreamPagesResponse::kFormField:
      return on_form_field(event.form_field());
    case officev1::StreamPagesResponse::EVENT_NOT_SET:
      return;
  }
}

void DoclingMapper::on_document_info(const officev1::DocumentInfo& info) {
  document_type_ = info.document_type();
  page_rects_.assign(info.page_rects().begin(), info.page_rects().end());
  if (document_.name().empty() && !info.document_id().empty()) {
    document_.set_name(info.document_id());
  }
  docv1::DocumentOrigin* origin = document_.mutable_origin();
  origin->set_mimetype(mime_for_extension(info.source_format()));
  if (!info.document_id().empty()) origin->set_filename(info.document_id());
  for (int index = 0; index < info.page_rects_size(); index++) {
    const officev1::PageRect& rect = info.page_rects(index);
    docv1::PageItem* page = &(*document_.mutable_pages())[index + 1];
    page->set_page_no(index + 1);
    page->mutable_size()->set_width(static_cast<double>(rect.width_twips()));
    page->mutable_size()->set_height(static_cast<double>(rect.height_twips()));
  }
}

void DoclingMapper::on_page_image(const officev1::PageImage& image) {
  docv1::PageItem* page = &(*document_.mutable_pages())[image.index() + 1];
  page->set_page_no(image.index() + 1);
  if (page->size().width() <= 0 && image.dpi() > 0) {
    page->mutable_size()->set_width(
        static_cast<double>(image.width_px()) * 1440.0 / image.dpi());
    page->mutable_size()->set_height(
        static_cast<double>(image.height_px()) * 1440.0 / image.dpi());
  }
  docv1::ImageRef* ref = page->mutable_image();
  ref->set_mimetype("image/png");
  ref->set_dpi(image.dpi());
  ref->mutable_size()->set_width(image.width_px());
  ref->mutable_size()->set_height(image.height_px());
  ref->set_uri(data_uri("image/png", image.png()));
}

void DoclingMapper::on_metadata(const officev1::DocumentMetadata& meta) {
  if (!meta.title().empty()) document_.set_name(meta.title());
  docv1::BaseMeta* body_meta = document_.mutable_body()->mutable_meta();
  auto* fields = body_meta->mutable_custom_fields();
  if (!meta.author().empty()) (*fields)["author"] = str_value(meta.author());
  if (!meta.subject().empty()) (*fields)["subject"] = str_value(meta.subject());
  if (!meta.keywords().empty()) {
    google::protobuf::Value list;
    for (const std::string& keyword : meta.keywords()) {
      *list.mutable_list_value()->add_values() = str_value(keyword);
    }
    (*fields)["keywords"] = list;
  }
  if (meta.created_epoch_ms() != 0) {
    (*fields)["created_epoch_ms"] =
        num_value(static_cast<double>(meta.created_epoch_ms()));
  }
  if (meta.modified_epoch_ms() != 0) {
    (*fields)["modified_epoch_ms"] =
        num_value(static_cast<double>(meta.modified_epoch_ms()));
  }
  if (!meta.modified_by().empty()) {
    (*fields)["modified_by"] = str_value(meta.modified_by());
  }
  if (!meta.generator().empty()) {
    (*fields)["generator"] = str_value(meta.generator());
  }
  if (meta.printed_epoch_ms() != 0) {
    (*fields)["printed_epoch_ms"] =
        num_value(static_cast<double>(meta.printed_epoch_ms()));
  }
  if (!meta.printed_by().empty()) {
    (*fields)["printed_by"] = str_value(meta.printed_by());
  }
  if (!meta.template_name().empty()) {
    (*fields)["template_name"] = str_value(meta.template_name());
  }
  if (!meta.statistics().empty()) {
    google::protobuf::Value stats;
    for (const auto& entry : meta.statistics()) {
      (*stats.mutable_struct_value()->mutable_fields())[entry.first] =
          num_value(static_cast<double>(entry.second));
    }
    (*fields)["statistics"] = stats;
  }
  if (!meta.user_properties().empty()) {
    google::protobuf::Value props;
    for (const officev1::UserProperty& prop : meta.user_properties()) {
      google::protobuf::Value value;
      switch (prop.value_case()) {
        case officev1::UserProperty::kText:
          value = str_value(prop.text());
          break;
        case officev1::UserProperty::kNumber:
          value = num_value(prop.number());
          break;
        case officev1::UserProperty::kFlag:
          value = bool_value(prop.flag());
          break;
        case officev1::UserProperty::kEpochMs:
          value = num_value(static_cast<double>(prop.epoch_ms()));
          break;
        case officev1::UserProperty::VALUE_NOT_SET:
          break;
      }
      (*props.mutable_struct_value()->mutable_fields())[prop.name()] = value;
    }
    (*fields)["user_properties"] = props;
  }
  if (!meta.language().empty()) {
    docv1::LanguageMetaField* language = body_meta->mutable_language();
    language->set_code_raw(meta.language());
    std::string subtag = meta.language().substr(0, meta.language().find('-'));
    std::transform(subtag.begin(), subtag.end(), subtag.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    docv1::HumanLanguageLabel code;
    if (docv1::HumanLanguageLabel_Parse("HUMAN_LANGUAGE_LABEL_" + subtag,
                                        &code)) {
      language->set_code(code);
    }
  }
}

void DoclingMapper::on_status(const officev1::RenderStatus& status) {
  for (const std::string& warning : status.warnings()) {
    warnings_.push_back(warning);
  }
  finished_ = true;
}

void DoclingMapper::on_paragraph(const officev1::Paragraph& paragraph) {
  std::string text = concat_runs(paragraph.runs());
  long long length = runs_length(paragraph.runs());
  long long span_start =
      paragraph.char_offset() >= 0 ? paragraph.char_offset() : 0;
  long long span_end = span_start + length;
  TextHandle handle;
  if (paragraph.style() == "Title") {
    handle = add_text(TextKind::kTitle, docv1::DOC_ITEM_LABEL_TITLE,
                      docv1::CONTENT_LAYER_BODY, "#/body");
  } else if (paragraph.outline_level() >= 1) {
    handle = add_text(TextKind::kSectionHeader,
                      docv1::DOC_ITEM_LABEL_SECTION_HEADER,
                      docv1::CONTENT_LAYER_BODY, "#/body");
    handle.item->mutable_section_header()->set_level(paragraph.outline_level());
  } else if (paragraph.list_level() >= 0) {
    handle = add_text(TextKind::kList, docv1::DOC_ITEM_LABEL_LIST_ITEM,
                      docv1::CONTENT_LAYER_BODY, "#/body");
  } else {
    handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                      docv1::CONTENT_LAYER_BODY, "#/body");
  }
  handle.base->set_text(text);
  handle.base->set_orig(text);
  set_uniform_formatting(paragraph.runs(), handle.base);
  apply_run_hyperlinks(paragraph.runs(), handle.base);
  if (!paragraph.line_rects().empty()) {
    add_line_prov(handle.base->mutable_prov(), paragraph.line_rects(),
                  span_start, span_end);
  } else {
    add_caret_prov(handle.base->mutable_prov(), paragraph.page_index(),
                   paragraph.start(), paragraph.end(), span_start, span_end);
  }
}

void DoclingMapper::on_table(const officev1::TableData& table) {
  docv1::TableItem* item = add_table(docv1::CONTENT_LAYER_BODY, "#/body",
                                     nullptr);
  fold_table(table, item);
  if (!table.line_rects().empty()) {
    add_line_prov(item->mutable_prov(), table.line_rects(), 0, 0);
  } else {
    add_caret_prov(item->mutable_prov(), table.page_index(), table.start(),
                   table.end(), 0, 0);
  }
}

void DoclingMapper::on_embedded_image(const officev1::EmbeddedImage& image) {
  std::string parent = "#/body";
  auto container = writer_groups_.find(image.group_path());
  if (container != writer_groups_.end()) parent = container->second;
  docv1::PictureItem* picture = add_picture(
      docv1::DOC_ITEM_LABEL_PICTURE, docv1::CONTENT_LAYER_BODY, parent,
      nullptr);
  if (!image.name().empty()) {
    (*picture->mutable_meta()->mutable_custom_fields())["name"] =
        str_value(image.name());
  }
  if (!image.data().empty()) {
    docv1::ImageRef* ref = picture->mutable_image();
    ref->set_mimetype(image.mime_type());
    ref->mutable_size()->set_width(static_cast<double>(image.width_twips()));
    ref->mutable_size()->set_height(static_cast<double>(image.height_twips()));
    ref->set_uri(data_uri(image.mime_type(), image.data()));
  }
  if (image.has_anchor()) {
    add_prov(picture->mutable_prov(), image.page_index(), false,
             static_cast<double>(image.anchor().x()),
             static_cast<double>(image.anchor().y()),
             static_cast<double>(image.anchor().x() + image.width_twips()),
             static_cast<double>(image.anchor().y() + image.height_twips()),
             0, 0);
  } else if (!image.line_rects().empty()) {
    add_line_prov(picture->mutable_prov(), image.line_rects(), 0, 0);
  }
}

void DoclingMapper::on_footnote(const officev1::Footnote& footnote) {
  TextHandle handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_FOOTNOTE,
                               docv1::CONTENT_LAYER_BODY, "#/body");
  std::string text = concat_runs(footnote.runs());
  handle.base->set_text(text);
  handle.base->set_orig(text);
  auto* fields = handle.base->mutable_meta()->mutable_custom_fields();
  if (!footnote.label().empty()) (*fields)["label"] = str_value(footnote.label());
  (*fields)["endnote"] = bool_value(footnote.endnote());
  apply_run_hyperlinks(footnote.runs(), handle.base);
  add_caret_prov(handle.base->mutable_prov(), footnote.page_index(),
                 footnote.anchor(), footnote.anchor(), 0,
                 runs_length(footnote.runs()));
}

void DoclingMapper::on_header_footer(const officev1::HeaderFooter& block) {
  docv1::DocItemLabel label = block.footer()
      ? docv1::DOC_ITEM_LABEL_PAGE_FOOTER
      : docv1::DOC_ITEM_LABEL_PAGE_HEADER;
  for (const officev1::Paragraph& paragraph : block.paragraphs()) {
    TextHandle handle = add_text(TextKind::kText, label,
                                 docv1::CONTENT_LAYER_FURNITURE, "#/furniture");
    std::string text = concat_runs(paragraph.runs());
    handle.base->set_text(text);
    handle.base->set_orig(text);
    set_uniform_formatting(paragraph.runs(), handle.base);
    apply_run_hyperlinks(paragraph.runs(), handle.base);
    (*handle.base->mutable_meta()->mutable_custom_fields())["page_style"] =
        str_value(block.page_style());
  }
}

void DoclingMapper::on_page_style(const officev1::PageStyleInfo& style) {
  google::protobuf::Value value;
  auto* fields = value.mutable_struct_value()->mutable_fields();
  (*fields)["width_twips"] = num_value(static_cast<double>(style.width_twips()));
  (*fields)["height_twips"] =
      num_value(static_cast<double>(style.height_twips()));
  (*fields)["margin_left_twips"] =
      num_value(static_cast<double>(style.margin_left_twips()));
  (*fields)["margin_right_twips"] =
      num_value(static_cast<double>(style.margin_right_twips()));
  (*fields)["margin_top_twips"] =
      num_value(static_cast<double>(style.margin_top_twips()));
  (*fields)["margin_bottom_twips"] =
      num_value(static_cast<double>(style.margin_bottom_twips()));
  (*fields)["columns"] = num_value(style.columns());
  (*document_.mutable_body()->mutable_meta()->mutable_custom_fields())
      ["page_style:" + style.name()] = value;
}

void DoclingMapper::on_document_index(const officev1::DocumentIndex& index) {
  TextHandle handle = add_text(TextKind::kText,
                               docv1::DOC_ITEM_LABEL_DOCUMENT_INDEX,
                               docv1::CONTENT_LAYER_BODY, "#/body");
  std::string text = concat_runs(index.runs());
  handle.base->set_text(text);
  handle.base->set_orig(text);
  auto* fields = handle.base->mutable_meta()->mutable_custom_fields();
  (*fields)["index_type"] = str_value(index.type());
  if (!index.title().empty()) (*fields)["title"] = str_value(index.title());
  apply_run_hyperlinks(index.runs(), handle.base);
  add_caret_prov(handle.base->mutable_prov(), index.page_index(),
                 index.anchor(), index.anchor(), 0, runs_length(index.runs()));
}

void DoclingMapper::on_drawing_shape(const officev1::DrawingShape& shape) {
  std::string parent = "#/body";
  auto container = draw_groups_.find({shape.page_index(), shape.group_path()});
  if (container != draw_groups_.end()) parent = container->second;
  double l = static_cast<double>(shape.position().x());
  double t = static_cast<double>(shape.position().y());
  double r = l + static_cast<double>(shape.width_twips());
  double b = t + static_cast<double>(shape.height_twips());
  if (shape.is_group()) {
    docv1::GroupItem* group = add_group(parent,
                                        docv1::GROUP_LABEL_PICTURE_AREA,
                                        shape.name(),
                                        docv1::CONTENT_LAYER_BODY);
    std::string child_path = shape.group_path().empty()
        ? std::to_string(shape.z_order())
        : shape.group_path() + "/" + std::to_string(shape.z_order());
    draw_groups_[{shape.page_index(), child_path}] = group->self_ref();
    return;
  }
  if (shape.has_text()) {
    TextHandle handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                                 docv1::CONTENT_LAYER_BODY, parent);
    std::string text = concat_runs(shape.runs());
    handle.base->set_text(text);
    handle.base->set_orig(text);
    set_uniform_formatting(shape.runs(), handle.base);
    apply_run_hyperlinks(shape.runs(), handle.base);
    (*handle.base->mutable_meta()->mutable_custom_fields())["shape_type"] =
        str_value(shape.shape_type());
    // Draw positions are page-local per part.
    add_prov(handle.base->mutable_prov(), shape.page_index(), true, l, t, r, b,
             0, runs_length(shape.runs()));
    return;
  }
  docv1::PictureItem* picture = add_picture(docv1::DOC_ITEM_LABEL_PICTURE,
                                            docv1::CONTENT_LAYER_BODY, parent,
                                            nullptr);
  auto* fields = picture->mutable_meta()->mutable_custom_fields();
  (*fields)["shape_type"] = str_value(shape.shape_type());
  if (!shape.name().empty()) (*fields)["name"] = str_value(shape.name());
  add_prov(picture->mutable_prov(), shape.page_index(), true, l, t, r, b, 0, 0);
}

void DoclingMapper::on_slide(const officev1::Slide& slide) {
  docv1::GroupItem* group = add_group("#/body", docv1::GROUP_LABEL_SLIDE,
                                      slide.name(), docv1::CONTENT_LAYER_BODY);
  auto* fields = group->mutable_meta()->mutable_custom_fields();
  (*fields)["layout"] = num_value(slide.layout());
  if (!slide.master_page_name().empty()) {
    (*fields)["master_page_name"] = str_value(slide.master_page_name());
  }
  slide_group_[slide.index()] = group->self_ref();
}

void DoclingMapper::on_slide_shape(const officev1::SlideShape& shape) {
  if (shape.is_empty_placeholder()) return;
  std::string parent = "#/body";
  auto group = slide_group_.find(shape.slide_index());
  if (group != slide_group_.end()) parent = group->second;
  docv1::ContentLayer layer = shape.notes() ? docv1::CONTENT_LAYER_NOTES
                                            : docv1::CONTENT_LAYER_BODY;
  // Notes shapes carry no slide-page provenance: their geometry is in
  // notes-page space, which has no PageImage.
  int prov_page = shape.notes() ? -1 : shape.slide_index();
  double l = static_cast<double>(shape.position().x());
  double t = static_cast<double>(shape.position().y());
  double r = l + static_cast<double>(shape.width_twips());
  double b = t + static_cast<double>(shape.height_twips());

  bool has_text = false;
  for (const officev1::SlideTextParagraph& paragraph : shape.paragraphs()) {
    if (!paragraph.runs().empty()) has_text = true;
  }
  if (!has_text) {
    if (ends_with(shape.shape_type(), "GraphicObjectShape")
        || ends_with(shape.shape_type(), "OLE2Shape")
        || ends_with(shape.shape_type(), "TableShape")
        || ends_with(shape.shape_type(), "MediaShape")) {
      docv1::PictureItem* picture = add_picture(docv1::DOC_ITEM_LABEL_PICTURE,
                                                layer, parent, nullptr);
      (*picture->mutable_meta()->mutable_custom_fields())["shape_type"] =
          str_value(shape.shape_type());
      add_prov(picture->mutable_prov(), prov_page, true, l, t, r, b, 0, 0);
    }
    return;
  }

  if (shape.placeholder_role() == officev1::PLACEHOLDER_ROLE_OUTLINE) {
    // Outline placeholders keep their per-paragraph depth: top-level lines
    // become section headers, deeper lines list items.
    for (const officev1::SlideTextParagraph& paragraph : shape.paragraphs()) {
      if (paragraph.runs().empty()) continue;
      TextHandle handle;
      if (paragraph.outline_depth() == 0) {
        handle = add_text(TextKind::kSectionHeader,
                          docv1::DOC_ITEM_LABEL_SECTION_HEADER, layer, parent);
        handle.item->mutable_section_header()->set_level(1);
      } else {
        handle = add_text(TextKind::kList, docv1::DOC_ITEM_LABEL_LIST_ITEM,
                          layer, parent);
      }
      std::string text = concat_runs(paragraph.runs());
      handle.base->set_text(text);
      handle.base->set_orig(text);
      set_uniform_formatting(paragraph.runs(), handle.base);
      apply_run_hyperlinks(paragraph.runs(), handle.base);
      add_prov(handle.base->mutable_prov(), prov_page, true, l, t, r, b, 0,
               runs_length(paragraph.runs()));
    }
    return;
  }

  TextHandle handle;
  if (shape.placeholder_role() == officev1::PLACEHOLDER_ROLE_TITLE) {
    handle = add_text(TextKind::kTitle, docv1::DOC_ITEM_LABEL_TITLE, layer,
                      parent);
  } else {
    handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT, layer,
                      parent);
  }
  std::string text;
  long long length = 0;
  for (const officev1::SlideTextParagraph& paragraph : shape.paragraphs()) {
    if (!text.empty()) text += "\n";
    text += concat_runs(paragraph.runs());
    length += runs_length(paragraph.runs());
  }
  handle.base->set_text(text);
  handle.base->set_orig(text);
  add_prov(handle.base->mutable_prov(), prov_page, true, l, t, r, b, 0, length);
}

void DoclingMapper::on_text_frame(const officev1::TextFrame& frame) {
  docv1::GroupItem* group = add_group("#/body", docv1::GROUP_LABEL_UNSPECIFIED,
                                      frame.name(), docv1::CONTENT_LAYER_BODY);
  auto* fields = group->mutable_meta()->mutable_custom_fields();
  if (!frame.chain_next().empty()) {
    (*fields)["chain_next"] = str_value(frame.chain_next());
  }
  if (!frame.chain_prev().empty()) {
    (*fields)["chain_prev"] = str_value(frame.chain_prev());
  }
  TextHandle handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                               docv1::CONTENT_LAYER_BODY, group->self_ref());
  std::string text = concat_runs(frame.runs());
  handle.base->set_text(text);
  handle.base->set_orig(text);
  set_uniform_formatting(frame.runs(), handle.base);
  apply_run_hyperlinks(frame.runs(), handle.base);
  if (frame.has_anchor()) {
    add_prov(handle.base->mutable_prov(), frame.page_index(), false,
             static_cast<double>(frame.anchor().x()),
             static_cast<double>(frame.anchor().y()),
             static_cast<double>(frame.anchor().x() + frame.width_twips()),
             static_cast<double>(frame.anchor().y() + frame.height_twips()),
             0, runs_length(frame.runs()));
  }
}

int DoclingMapper::page_for_point(double x, double y) const {
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

void DoclingMapper::on_shape(const officev1::Shape& shape) {
  std::string parent = "#/body";
  auto container = writer_groups_.find(shape.group_path());
  if (container != writer_groups_.end()) parent = container->second;

  if (shape.is_group()) {
    docv1::GroupItem* group = add_group(parent,
                                        docv1::GROUP_LABEL_PICTURE_AREA,
                                        shape.name(),
                                        docv1::CONTENT_LAYER_BODY);
    (*group->mutable_meta()->mutable_custom_fields())["shape_type"] =
        str_value(shape.shape_type());
    std::string child_path = shape.group_path().empty()
        ? std::to_string(shape.z_order())
        : shape.group_path() + "/" + std::to_string(shape.z_order());
    writer_groups_[child_path] = group->self_ref();
    return;
  }

  docv1::GroupItem* group = add_group(parent, docv1::GROUP_LABEL_UNSPECIFIED,
                                      shape.name(), docv1::CONTENT_LAYER_BODY);
  auto* fields = group->mutable_meta()->mutable_custom_fields();
  (*fields)["shape_type"] = str_value(shape.shape_type());
  if (!shape.chain_next().empty()) {
    (*fields)["chain_next"] = str_value(shape.chain_next());
  }
  if (!shape.chain_prev().empty()) {
    (*fields)["chain_prev"] = str_value(shape.chain_prev());
  }
  TextHandle handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                               docv1::CONTENT_LAYER_BODY, group->self_ref());
  std::string text = concat_runs(shape.runs());
  handle.base->set_text(text);
  handle.base->set_orig(text);
  set_uniform_formatting(shape.runs(), handle.base);
  apply_run_hyperlinks(shape.runs(), handle.base);
  if (shape.has_anchor()) {
    add_prov(handle.base->mutable_prov(), shape.page_index(), false,
             static_cast<double>(shape.anchor().x()),
             static_cast<double>(shape.anchor().y()),
             static_cast<double>(shape.anchor().x() + shape.width_twips()),
             static_cast<double>(shape.anchor().y() + shape.height_twips()),
             0, runs_length(shape.runs()));
  } else if (shape.has_position()) {
    // Group children carry a model position instead of a caret anchor; the
    // page resolves from the position, which shares the document-absolute
    // space of the page rectangles.
    double l = static_cast<double>(shape.position().x());
    double t = static_cast<double>(shape.position().y());
    double r = l + static_cast<double>(shape.width_twips());
    double b = t + static_cast<double>(shape.height_twips());
    add_prov(handle.base->mutable_prov(),
             page_for_point((l + r) / 2, (t + b) / 2), false, l, t, r, b,
             0, runs_length(shape.runs()));
  }
}

void DoclingMapper::on_embedded_object(const officev1::EmbeddedObject& object) {
  // Geometry: Writer text-anchored objects carry a document-absolute caret
  // anchor; draw-page objects carry a page-local position.
  bool page_local = !object.has_anchor();
  double l;
  double t;
  if (object.has_anchor()) {
    l = static_cast<double>(object.anchor().x());
    t = static_cast<double>(object.anchor().y());
  } else {
    l = static_cast<double>(object.position().x());
    t = static_cast<double>(object.position().y());
  }
  double r = l + static_cast<double>(object.width_twips());
  double b = t + static_cast<double>(object.height_twips());

  if (object.kind() == officev1::EMBEDDED_OBJECT_KIND_FORMULA) {
    TextHandle handle = add_text(TextKind::kFormula,
                                 docv1::DOC_ITEM_LABEL_FORMULA,
                                 docv1::CONTENT_LAYER_BODY, "#/body");
    handle.base->set_text(object.formula());
    handle.base->set_orig(object.formula());
    if (!object.name().empty()) {
      (*handle.base->mutable_meta()->mutable_custom_fields())["name"] =
          str_value(object.name());
    }
    add_prov(handle.base->mutable_prov(), object.page_index(), page_local, l, t,
             r, b, 0, static_cast<long long>(object.formula().size()));
    return;
  }

  if (object.kind() == officev1::EMBEDDED_OBJECT_KIND_SPREADSHEET) {
    docv1::TableItem* item = add_table(docv1::CONTENT_LAYER_BODY, "#/body",
                                       nullptr);
    fold_table(object.inner_table(), item);
    if (!object.name().empty()) {
      (*item->mutable_meta()->mutable_custom_fields())["name"] =
          str_value(object.name());
    }
    add_prov(item->mutable_prov(), object.page_index(), page_local, l, t, r, b,
             0, 0);
    return;
  }

  bool is_chart = object.kind() == officev1::EMBEDDED_OBJECT_KIND_CHART;
  docv1::PictureItem* picture = add_picture(
      is_chart ? docv1::DOC_ITEM_LABEL_CHART : docv1::DOC_ITEM_LABEL_PICTURE,
      docv1::CONTENT_LAYER_BODY, "#/body", nullptr);
  auto* fields = picture->mutable_meta()->mutable_custom_fields();
  if (!object.name().empty()) (*fields)["name"] = str_value(object.name());
  if (!object.clsid().empty()) (*fields)["clsid"] = str_value(object.clsid());
  if (!object.replacement_image().empty()) {
    docv1::ImageRef* ref = picture->mutable_image();
    ref->set_mimetype(object.replacement_mime_type());
    ref->mutable_size()->set_width(static_cast<double>(object.width_twips()));
    ref->mutable_size()->set_height(static_cast<double>(object.height_twips()));
    ref->set_uri(data_uri(object.replacement_mime_type(),
                          object.replacement_image()));
  }
  add_prov(picture->mutable_prov(), object.page_index(), page_local, l, t, r, b,
           0, 0);
  if (!is_chart) return;

  const officev1::EmbeddedChart& chart = object.chart();
  const auto& series = chart.series();
  switch (chart.kind()) {
    case officev1::EMBEDDED_CHART_KIND_BAR:
    case officev1::EMBEDDED_CHART_KIND_COLUMN: {
      if (series.empty()) break;
      docv1::PictureBarChartData* bars =
          picture->add_annotations()->mutable_bar_chart();
      bars->set_kind("bar_chart_data");
      bars->set_title(chart.title());
      bars->set_x_axis_label(chart.x_axis_title());
      bars->set_y_axis_label(chart.y_axis_title());
      for (int i = 0; i < series[0].values_y_size(); i++) {
        docv1::ChartBar* bar = bars->add_bars();
        bar->set_label(i < chart.categories_size() ? chart.categories(i)
                                                   : std::to_string(i + 1));
        bar->set_values(series[0].values_y(i));
      }
      break;
    }
    case officev1::EMBEDDED_CHART_KIND_LINE:
    case officev1::EMBEDDED_CHART_KIND_AREA: {
      docv1::PictureLineChartData* lines =
          picture->add_annotations()->mutable_line_chart();
      lines->set_kind("line_chart_data");
      lines->set_title(chart.title());
      lines->set_x_axis_label(chart.x_axis_title());
      lines->set_y_axis_label(chart.y_axis_title());
      for (const officev1::EmbeddedChartSeries& one : series) {
        docv1::ChartLine* line = lines->add_lines();
        line->set_label(one.label());
        for (int i = 0; i < one.values_y_size(); i++) {
          docv1::FloatPair* pair = line->add_values();
          pair->set_first(i < one.values_x_size() ? one.values_x(i)
                                                  : static_cast<double>(i));
          pair->set_second(one.values_y(i));
        }
      }
      break;
    }
    case officev1::EMBEDDED_CHART_KIND_PIE: {
      if (series.empty()) break;
      docv1::PicturePieChartData* pie =
          picture->add_annotations()->mutable_pie_chart();
      pie->set_kind("pie_chart_data");
      pie->set_title(chart.title());
      for (int i = 0; i < series[0].values_y_size(); i++) {
        docv1::ChartSlice* slice = pie->add_slices();
        slice->set_label(i < chart.categories_size() ? chart.categories(i)
                                                     : std::to_string(i + 1));
        slice->set_value(series[0].values_y(i));
      }
      break;
    }
    case officev1::EMBEDDED_CHART_KIND_SCATTER:
    case officev1::EMBEDDED_CHART_KIND_BUBBLE: {
      docv1::PictureScatterChartData* scatter =
          picture->add_annotations()->mutable_scatter_chart();
      scatter->set_kind("scatter_chart_data");
      scatter->set_title(chart.title());
      scatter->set_x_axis_label(chart.x_axis_title());
      scatter->set_y_axis_label(chart.y_axis_title());
      for (const officev1::EmbeddedChartSeries& one : series) {
        int points = std::min(one.values_x_size(), one.values_y_size());
        for (int i = 0; i < points; i++) {
          docv1::FloatPair* pair =
              scatter->add_points()->mutable_value();
          pair->set_first(one.values_x(i));
          pair->set_second(one.values_y(i));
        }
      }
      break;
    }
    default:
      break;
  }
  // The tabular projection is always attached: any chart family stays
  // representable, including kinds with no typed variant above.
  docv1::PictureTabularChartData* tabular =
      picture->add_annotations()->mutable_tabular_chart();
  tabular->set_kind("tabular_chart_data");
  tabular->set_title(chart.title());
  docv1::TableItem scratch;
  fold_table(chart.tabular(), &scratch);
  *tabular->mutable_chart_data() = scratch.data();
}

void DoclingMapper::on_sheet(const officev1::Sheet& sheet) {
  docv1::ContentLayer layer = sheet.visible()
      ? docv1::CONTENT_LAYER_BODY
      : docv1::CONTENT_LAYER_INVISIBLE;
  sheet_layer_[sheet.index()] = layer;
  docv1::GroupItem* group = add_group("#/body", docv1::GROUP_LABEL_SHEET,
                                      sheet.name(), layer);
  auto* fields = group->mutable_meta()->mutable_custom_fields();
  (*fields)["visible"] = bool_value(sheet.visible());
  if (sheet.tab_color_rgb() >= 0) {
    (*fields)["tab_color_rgb"] = num_value(sheet.tab_color_rgb());
  }
  if (!sheet.print_areas().empty()) {
    google::protobuf::Value areas;
    for (const officev1::SheetRangeRef& area : sheet.print_areas()) {
      *areas.mutable_list_value()->add_values() = str_value(range_a1(area));
    }
    (*fields)["print_areas"] = areas;
  }
  sheet_group_[sheet.index()] = group->self_ref();

  // The sheet's cell grid folds into one TableItem in absolute row and
  // column offsets, so cell addresses survive the mapping.
  sheet_table_[sheet.index()] = document_.tables_size();
  docv1::TableItem* table = add_table(layer, group->self_ref(), nullptr);
  table->mutable_data()->set_num_rows(sheet.used_end_row() + 1);
  table->mutable_data()->set_num_cols(sheet.used_end_column() + 1);
  add_prov(table->mutable_prov(), sheet.index(), true, 0, 0, 0, 0, 0, 0);
}

void DoclingMapper::on_sheet_row(const officev1::SheetRow& row) {
  auto found = sheet_table_.find(row.sheet_index());
  if (found == sheet_table_.end()) return;
  docv1::TableItem* table = document_.mutable_tables(found->second);
  for (const officev1::SheetCell& cell : row.cells()) {
    docv1::TableCell* out = table->mutable_data()->add_table_cells();
    out->set_start_row_offset_idx(row.row());
    out->set_end_row_offset_idx(row.row() + std::max(1, cell.merged_rows()));
    out->set_start_col_offset_idx(cell.column());
    out->set_end_col_offset_idx(cell.column()
                                + std::max(1, cell.merged_columns()));
    out->set_row_span(std::max(1, cell.merged_rows()));
    out->set_col_span(std::max(1, cell.merged_columns()));
    out->set_text(cell.display());
    // TableCell has no numeric or formula slot; numbers stay numbers in the
    // table meta, keyed by the cell's A1 name.
    if (cell.type() == officev1::SHEET_CELL_TYPE_VALUE) {
      (*table->mutable_meta()->mutable_custom_fields())
          [a1_name(row.row(), cell.column())] = num_value(cell.number());
    } else if (cell.type() == officev1::SHEET_CELL_TYPE_FORMULA) {
      google::protobuf::Value value;
      auto* fields = value.mutable_struct_value()->mutable_fields();
      (*fields)["formula"] = str_value(cell.formula());
      (*fields)["number"] = num_value(cell.number());
      (*table->mutable_meta()->mutable_custom_fields())
          [a1_name(row.row(), cell.column())] = value;
    }
  }
}

void DoclingMapper::on_sheet_named_range(
    const officev1::SheetNamedRange& range) {
  google::protobuf::Value value;
  auto* fields = value.mutable_struct_value()->mutable_fields();
  (*fields)["content"] = str_value(range.content());
  (*fields)["type_flags"] = num_value(range.type_flags());
  (*document_.mutable_body()->mutable_meta()->mutable_custom_fields())
      ["named_range:" + range.name()] = value;
}

void DoclingMapper::on_sheet_database_range(
    const officev1::SheetDatabaseRange& range) {
  google::protobuf::Value value;
  auto* fields = value.mutable_struct_value()->mutable_fields();
  (*fields)["sheet_index"] = num_value(range.sheet_index());
  (*fields)["range"] = str_value(range_a1(range.range()));
  (*fields)["contains_header"] = bool_value(range.contains_header());
  (*fields)["totals_row"] = bool_value(range.totals_row());
  (*fields)["auto_filter"] = bool_value(range.auto_filter());
  (*document_.mutable_body()->mutable_meta()->mutable_custom_fields())
      ["database_range:" + range.name()] = value;
}

void DoclingMapper::on_sheet_cell_comment(
    const officev1::SheetCellComment& comment) {
  std::string sheet_ref = "#/body";
  auto group = sheet_group_.find(comment.sheet_index());
  if (group != sheet_group_.end()) sheet_ref = group->second;
  docv1::ContentLayer layer = docv1::CONTENT_LAYER_BODY;
  auto sheet_layer = sheet_layer_.find(comment.sheet_index());
  if (sheet_layer != sheet_layer_.end()) layer = sheet_layer->second;
  auto comments = sheet_comments_.find(comment.sheet_index());
  if (comments == sheet_comments_.end()) {
    docv1::GroupItem* section = add_group(
        sheet_ref, docv1::GROUP_LABEL_COMMENT_SECTION, "comments", layer);
    comments = sheet_comments_
        .emplace(comment.sheet_index(), section->self_ref()).first;
  }
  TextHandle handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                               layer, comments->second);
  handle.base->set_text(comment.text());
  handle.base->set_orig(comment.text());
  auto* fields = handle.base->mutable_meta()->mutable_custom_fields();
  (*fields)["cell"] = str_value(a1_name(comment.row(), comment.column()));
  if (!comment.author().empty()) {
    (*fields)["author"] = str_value(comment.author());
  }
  if (!comment.date().empty()) (*fields)["date"] = str_value(comment.date());
  (*fields)["visible"] = bool_value(comment.visible());
  auto table = sheet_table_.find(comment.sheet_index());
  if (table != sheet_table_.end()) {
    document_.mutable_tables(table->second)->add_comments()->set_ref(
        handle.ref);
  }
}

void DoclingMapper::on_sheet_chart(const officev1::SheetChart& chart) {
  std::string sheet_ref = "#/body";
  auto group = sheet_group_.find(chart.sheet_index());
  if (group != sheet_group_.end()) sheet_ref = group->second;
  docv1::ContentLayer layer = docv1::CONTENT_LAYER_BODY;
  auto sheet_layer = sheet_layer_.find(chart.sheet_index());
  if (sheet_layer != sheet_layer_.end()) layer = sheet_layer->second;
  docv1::PictureItem* picture = add_picture(docv1::DOC_ITEM_LABEL_CHART, layer,
                                            sheet_ref, nullptr);
  auto* fields = picture->mutable_meta()->mutable_custom_fields();
  (*fields)["name"] = str_value(chart.name());
  if (!chart.ranges().empty()) {
    google::protobuf::Value ranges;
    for (const officev1::SheetRangeRef& range : chart.ranges()) {
      *ranges.mutable_list_value()->add_values() = str_value(range_a1(range));
    }
    (*fields)["source_ranges"] = ranges;
  }
  (*fields)["has_column_headers"] = bool_value(chart.has_column_headers());
  (*fields)["has_row_headers"] = bool_value(chart.has_row_headers());
  add_prov(picture->mutable_prov(), chart.sheet_index(), true, 0, 0, 0, 0, 0,
           0);
}

void DoclingMapper::on_sheet_pivot_table(
    const officev1::SheetPivotTable& pivot) {
  std::string sheet_ref = "#/body";
  auto group = sheet_group_.find(pivot.sheet_index());
  if (group != sheet_group_.end()) sheet_ref = group->second;
  docv1::ContentLayer layer = docv1::CONTENT_LAYER_BODY;
  auto sheet_layer = sheet_layer_.find(pivot.sheet_index());
  if (sheet_layer != sheet_layer_.end()) layer = sheet_layer->second;
  docv1::TableItem* table = add_table(layer, sheet_ref, nullptr);
  const officev1::SheetRangeRef& output = pivot.output_range();
  table->mutable_data()->set_num_rows(output.end_row() - output.start_row()
                                      + 1);
  table->mutable_data()->set_num_cols(output.end_column()
                                      - output.start_column() + 1);
  auto* fields = table->mutable_meta()->mutable_custom_fields();
  (*fields)["pivot_table"] = str_value(pivot.name());
  (*fields)["source_range"] = str_value(range_a1(pivot.source_range()));
  (*fields)["output_range"] = str_value(range_a1(output));
  auto add_list = [&](const char* key, const auto& names) {
    if (names.empty()) return;
    google::protobuf::Value list;
    for (const std::string& name : names) {
      *list.mutable_list_value()->add_values() = str_value(name);
    }
    (*fields)[key] = list;
  };
  add_list("row_fields", pivot.row_fields());
  add_list("column_fields", pivot.column_fields());
  add_list("data_fields", pivot.data_fields());
  add_list("page_fields", pivot.page_fields());
  add_prov(table->mutable_prov(), pivot.sheet_index(), true, 0, 0, 0, 0, 0, 0);
}

void DoclingMapper::on_comment(const officev1::Comment& comment) {
  if (comments_group_ref_.empty()) {
    comments_group_ref_ =
        add_group("#/furniture", docv1::GROUP_LABEL_COMMENT_SECTION,
                  "comments", docv1::CONTENT_LAYER_FURNITURE)
            ->self_ref();
  }
  TextHandle handle = add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT,
                               docv1::CONTENT_LAYER_FURNITURE,
                               comments_group_ref_);
  std::string text =
      !comment.text().empty() ? comment.text() : concat_runs(comment.runs());
  handle.base->set_text(text);
  handle.base->set_orig(text);
  auto* fields = handle.base->mutable_meta()->mutable_custom_fields();
  if (!comment.name().empty()) (*fields)["name"] = str_value(comment.name());
  if (!comment.author().empty()) {
    (*fields)["author"] = str_value(comment.author());
  }
  if (!comment.initials().empty()) {
    (*fields)["initials"] = str_value(comment.initials());
  }
  if (comment.epoch_ms() != 0) {
    (*fields)["date_ms"] = num_value(static_cast<double>(comment.epoch_ms()));
  }
  if (comment.resolved()) (*fields)["resolved"] = bool_value(true);
  if (!comment.parent_name().empty()) {
    (*fields)["reply_to"] = str_value(comment.parent_name());
  }
  if (comment.char_start() >= 0) {
    (*fields)["char_start"] =
        num_value(static_cast<double>(comment.char_start()));
    (*fields)["char_end"] = num_value(static_cast<double>(comment.char_end()));
  }
  if (!comment.anchored_text().empty()) {
    (*fields)["anchored_text"] = str_value(comment.anchored_text());
  }
  add_caret_prov(handle.base->mutable_prov(), comment.page_index(),
                 comment.anchor(), comment.anchor(), 0,
                 runs_length(comment.runs()));
}

void DoclingMapper::on_tracked_change(const officev1::TrackedChange& change) {
  // Tracked changes annotate spans of the body text rather than adding
  // display text of their own, so they ride the body metadata; the changed
  // text itself is already in the body items when the change is displayed.
  google::protobuf::Value value;
  google::protobuf::Struct* fields = value.mutable_struct_value();
  (*fields->mutable_fields())["kind"] = str_value(
      change.kind_name().empty() ? "Unspecified" : change.kind_name());
  if (!change.author().empty()) {
    (*fields->mutable_fields())["author"] = str_value(change.author());
  }
  if (change.epoch_ms() != 0) {
    (*fields->mutable_fields())["date_ms"] =
        num_value(static_cast<double>(change.epoch_ms()));
  }
  if (!change.comment().empty()) {
    (*fields->mutable_fields())["comment"] = str_value(change.comment());
  }
  (*fields->mutable_fields())["char_start"] =
      num_value(static_cast<double>(change.char_start()));
  (*fields->mutable_fields())["char_end"] =
      num_value(static_cast<double>(change.char_end()));
  if (!change.changed_text().empty()) {
    (*fields->mutable_fields())["text"] = str_value(change.changed_text());
  }
  if (change.has_successor()) {
    google::protobuf::Struct* successor =
        (*fields->mutable_fields())["supersedes"].mutable_struct_value();
    (*successor->mutable_fields())["kind"] =
        str_value(change.successor().kind_name());
    if (!change.successor().author().empty()) {
      (*successor->mutable_fields())["author"] =
          str_value(change.successor().author());
    }
    if (change.successor().epoch_ms() != 0) {
      (*successor->mutable_fields())["date_ms"] =
          num_value(static_cast<double>(change.successor().epoch_ms()));
    }
  }
  (*document_.mutable_body()->mutable_meta()->mutable_custom_fields())
      ["tracked_change:" + std::to_string(change.index())] = std::move(value);
}

void DoclingMapper::on_bookmark(const officev1::Bookmark& bookmark) {
  // Bookmarks name spans of the body text; like tracked changes they ride
  // the body metadata instead of adding display text.
  google::protobuf::Value value;
  google::protobuf::Struct* fields = value.mutable_struct_value();
  (*fields->mutable_fields())["char_start"] =
      num_value(static_cast<double>(bookmark.char_start()));
  (*fields->mutable_fields())["char_end"] =
      num_value(static_cast<double>(bookmark.char_end()));
  if (bookmark.page_index() >= 0) {
    (*fields->mutable_fields())["page"] =
        num_value(static_cast<double>(bookmark.page_index() + 1));
  }
  if (!bookmark.covered_text().empty()) {
    (*fields->mutable_fields())["text"] = str_value(bookmark.covered_text());
  }
  (*document_.mutable_body()->mutable_meta()->mutable_custom_fields())
      ["bookmark:" + bookmark.name()] = std::move(value);
}

void DoclingMapper::on_form_field(const officev1::FormField& field) {
  if (form_fields_group_ref_.empty()) {
    form_fields_group_ref_ =
        add_group("#/body", docv1::GROUP_LABEL_FORM_AREA, "form_fields",
                  docv1::CONTENT_LAYER_BODY)
            ->self_ref();
  }
  docv1::DocItemLabel label = docv1::DOC_ITEM_LABEL_TEXT;
  if (field.kind() == officev1::FORM_FIELD_KIND_CHECKBOX) {
    label = field.checked() ? docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED
                            : docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED;
  }
  TextHandle handle = add_text(TextKind::kText, label,
                               docv1::CONTENT_LAYER_BODY,
                               form_fields_group_ref_);
  std::string text = !field.text().empty() ? field.text() : field.label();
  handle.base->set_text(text);
  handle.base->set_orig(text);
  auto* fields = handle.base->mutable_meta()->mutable_custom_fields();
  if (!field.field_type().empty()) {
    (*fields)["field_type"] = str_value(field.field_type());
  }
  if (!field.name().empty()) (*fields)["name"] = str_value(field.name());
  if (!field.label().empty()) (*fields)["label"] = str_value(field.label());
  if (field.control()) (*fields)["control"] = bool_value(true);
  if (field.kind() == officev1::FORM_FIELD_KIND_CHECKBOX) {
    (*fields)["checked"] = bool_value(field.checked());
  }
  if (field.selected_index() >= 0) {
    (*fields)["selected_index"] =
        num_value(static_cast<double>(field.selected_index()));
  }
  if (!field.list_entries().empty()) {
    google::protobuf::Value entries;
    for (const std::string& entry : field.list_entries()) {
      *entries.mutable_list_value()->add_values() = str_value(entry);
    }
    (*fields)["list_entries"] = entries;
  }
  if (field.char_start() >= 0) {
    (*fields)["char_start"] =
        num_value(static_cast<double>(field.char_start()));
    (*fields)["char_end"] = num_value(static_cast<double>(field.char_end()));
  }
  for (const officev1::FormFieldParameter& parameter : field.parameters()) {
    google::protobuf::Value parameter_value;
    switch (parameter.value_case()) {
      case officev1::FormFieldParameter::kBoolValue:
        parameter_value = bool_value(parameter.bool_value());
        break;
      case officev1::FormFieldParameter::kIntValue:
        parameter_value =
            num_value(static_cast<double>(parameter.int_value()));
        break;
      case officev1::FormFieldParameter::kDoubleValue:
        parameter_value = num_value(parameter.double_value());
        break;
      case officev1::FormFieldParameter::kStringValue:
        parameter_value = str_value(parameter.string_value());
        break;
      case officev1::FormFieldParameter::VALUE_NOT_SET:
        if (!parameter.string_list().empty()) {
          for (const std::string& entry : parameter.string_list()) {
            *parameter_value.mutable_list_value()->add_values() =
                str_value(entry);
          }
        } else {
          parameter_value.set_null_value(google::protobuf::NULL_VALUE);
        }
        break;
    }
    (*fields)["param:" + parameter.name()] = parameter_value;
  }
  if (field.control() && field.width_twips() > 0 && field.has_anchor()) {
    add_prov(handle.base->mutable_prov(), field.page_index(), false,
             static_cast<double>(field.anchor().x()),
             static_cast<double>(field.anchor().y()),
             static_cast<double>(field.anchor().x() + field.width_twips()),
             static_cast<double>(field.anchor().y() + field.height_twips()),
             0, 0);
  } else {
    add_caret_prov(handle.base->mutable_prov(), field.page_index(),
                   field.anchor(), field.anchor(), 0, 0);
  }
}

namespace {

const docv1::TextItemBase* text_base(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle: return &item.title().base();
    case docv1::BaseTextItem::kSectionHeader:
      return &item.section_header().base();
    case docv1::BaseTextItem::kListItem: return &item.list_item().base();
    case docv1::BaseTextItem::kFormula: return &item.formula().base();
    case docv1::BaseTextItem::kText: return &item.text().base();
    case docv1::BaseTextItem::kFieldHeading:
      return &item.field_heading().base();
    case docv1::BaseTextItem::kFieldValue: return &item.field_value().base();
    case docv1::BaseTextItem::kCode:
    case docv1::BaseTextItem::ITEM_NOT_SET:
      return nullptr;
  }
  return nullptr;
}

}  // namespace

std::vector<std::string> docling_integrity_errors(
    const docv1::Document& document) {
  std::vector<std::string> errors;
  std::set<std::string> refs = {"#/body", "#/furniture"};
  // (item ref, parent ref) pairs and every children list, gathered in one
  // walk so parents can be validated against their children afterwards.
  std::vector<std::pair<std::string, std::string>> parents;
  std::map<std::string, std::set<std::string>> children;

  auto collect = [&](const std::string& self_ref,
                     const google::protobuf::RepeatedPtrField<docv1::RefItem>&
                         child_refs,
                     bool has_parent, const std::string& parent_ref) {
    if (self_ref.empty()) {
      errors.push_back("item with empty self_ref");
      return;
    }
    if (!refs.insert(self_ref).second) {
      errors.push_back("duplicate self_ref " + self_ref);
    }
    for (const docv1::RefItem& child : child_refs) {
      children[self_ref].insert(child.ref());
    }
    if (has_parent) parents.emplace_back(self_ref, parent_ref);
  };

  for (const docv1::RefItem& child : document.body().children()) {
    children["#/body"].insert(child.ref());
  }
  for (const docv1::RefItem& child : document.furniture().children()) {
    children["#/furniture"].insert(child.ref());
  }
  for (const docv1::GroupItem& group : document.groups()) {
    collect(group.self_ref(), group.children(), group.has_parent(),
            group.parent().ref());
  }
  for (const docv1::BaseTextItem& item : document.texts()) {
    const docv1::TextItemBase* base = text_base(item);
    if (base == nullptr) {
      errors.push_back("text item with unset variant");
      continue;
    }
    collect(base->self_ref(), base->children(), base->has_parent(),
            base->parent().ref());
  }
  for (const docv1::PictureItem& picture : document.pictures()) {
    collect(picture.self_ref(), picture.children(), picture.has_parent(),
            picture.parent().ref());
  }
  for (const docv1::TableItem& table : document.tables()) {
    collect(table.self_ref(), table.children(), table.has_parent(),
            table.parent().ref());
  }

  for (const auto& entry : children) {
    for (const std::string& child : entry.second) {
      if (refs.find(child) == refs.end()) {
        errors.push_back("child " + child + " of " + entry.first
                         + " does not resolve");
      }
    }
  }
  for (const auto& parent : parents) {
    if (refs.find(parent.second) == refs.end()) {
      errors.push_back("parent " + parent.second + " of " + parent.first
                       + " does not resolve");
      continue;
    }
    auto listed = children.find(parent.second);
    if (listed == children.end()
        || listed->second.find(parent.first) == listed->second.end()) {
      errors.push_back("parent " + parent.second + " does not list "
                       + parent.first + " as a child");
    }
  }
  for (const docv1::TableItem& table : document.tables()) {
    for (const docv1::FineRef& comment : table.comments()) {
      if (refs.find(comment.ref()) == refs.end()) {
        errors.push_back("comment ref " + comment.ref() + " of "
                         + table.self_ref() + " does not resolve");
      }
    }
  }
  return errors;
}

}  // namespace grparse
