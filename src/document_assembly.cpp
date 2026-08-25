#include "grparse/document_assembly.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "grparse/base64.h"
#include "grparse/reading_order.h"
#include "grparse/region_geometry.h"
#include "grparse/table_structure.h"
#include "grparse/text_geometry.h"

namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

// Attribution when a detector reached assembly without naming itself (test
// doubles and older callers).
const std::string kUnnamedLayoutModel = "layout";

void set_bounding_box(const AxisAlignedBox& box, pipestream::document::v1::BoundingBox* output) {
  output->set_l(box.left);
  output->set_t(box.top);
  output->set_r(box.right);
  output->set_b(box.bottom);
  output->set_coord_origin(pipestream::document::v1::COORD_ORIGIN_TOPLEFT);
}

// Every emitted item names the collector and the engine that produced it;
// additive merges with other collectors' output rely on this attribution to
// never collide silently.
void add_collector_source(const std::string& model, std::optional<float> confidence,
                          google::protobuf::RepeatedPtrField<pipestream::document::v1::SourceType>* source) {
  auto* collector = source->Add()->mutable_collector();
  collector->set_collector("grparse");
  collector->set_model(model);
  if (confidence.has_value()) collector->set_confidence(*confidence);
}

pipestream::parse::v1::TextSource text_source_for(const OcrPage& page, const OcrLine& line) {
  if (line.origin.has_value()) {
    return *line.origin == TextOrigin::kDigitalPdf ? pipestream::parse::v1::TEXT_SOURCE_DIGITAL_PDF
                                                   : pipestream::parse::v1::TEXT_SOURCE_OCR;
  }
  switch (page.source) {
    case OcrPage::Source::kDigitalPdf:
      return pipestream::parse::v1::TEXT_SOURCE_DIGITAL_PDF;
    case OcrPage::Source::kMerged:
      // Prefer OCR label only when origin is missing; merged pages should set per-line origin.
      return pipestream::parse::v1::TEXT_SOURCE_OCR;
    case OcrPage::Source::kOcr:
    default:
      return pipestream::parse::v1::TEXT_SOURCE_OCR;
  }
}

// Region label -> document item label for the text lines inside it.  Covers
// both detectors' vocabularies; lines inside table/picture regions keep TEXT,
// because the region itself is emitted as a TableItem/PictureItem and
// cell/caption structure is later work.
pipestream::document::v1::DocItemLabel label_for_region(const std::string& label) {
  namespace docv1 = pipestream::document::v1;
  static const std::unordered_map<std::string, docv1::DocItemLabel> kLabels = {
      {"caption", docv1::DOC_ITEM_LABEL_CAPTION},
      {"checkbox_selected", docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED},
      {"checkbox_unselected", docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED},
      {"code", docv1::DOC_ITEM_LABEL_CODE},
      {"document_index", docv1::DOC_ITEM_LABEL_DOCUMENT_INDEX},
      {"footnote", docv1::DOC_ITEM_LABEL_FOOTNOTE},
      {"form", docv1::DOC_ITEM_LABEL_FORM},
      {"formula", docv1::DOC_ITEM_LABEL_FORMULA},
      {"key_value_region", docv1::DOC_ITEM_LABEL_KEY_VALUE_REGION},
      {"list", docv1::DOC_ITEM_LABEL_LIST_ITEM},
      {"list_item", docv1::DOC_ITEM_LABEL_LIST_ITEM},
      {"page_footer", docv1::DOC_ITEM_LABEL_PAGE_FOOTER},
      {"page_header", docv1::DOC_ITEM_LABEL_PAGE_HEADER},
      {"section_header", docv1::DOC_ITEM_LABEL_SECTION_HEADER},
      {"title", docv1::DOC_ITEM_LABEL_TITLE},
  };
  const auto found = kLabels.find(label);
  return found == kLabels.end() ? docv1::DOC_ITEM_LABEL_TEXT : found->second;
}

// Running headers and footers are page furniture, not body prose: they carry
// the furniture content layer and hang off the furniture group instead of
// #/body, so renderers that walk the body never fold a page number into the
// running text.
bool is_furniture_region(const LayoutRegion* region) {
  return region != nullptr && (region->label == "page_header" || region->label == "page_footer");
}

void set_region_bounding_box(const LayoutRegion& region, pipestream::document::v1::BoundingBox* output) {
  output->set_l(region.left);
  output->set_t(region.top);
  output->set_r(region.right);
  output->set_b(region.bottom);
  output->set_coord_origin(pipestream::document::v1::COORD_ORIGIN_TOPLEFT);
}

// Big-endian 32-bit read for the PNG IHDR dimensions.
uint32_t read_be32(const unsigned char* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

// Embed a captured crop as a data URI.  The pixel size comes from the PNG
// IHDR itself, which is authoritative regardless of the page's coordinate
// space (digital pages measure in PDF points, crops in raster pixels).
void set_picture_image(const std::vector<unsigned char>& png, pipestream::document::v1::ImageRef* image) {
  image->set_mimetype("image/png");
  // IHDR starts at byte 8; width and height are its first two fields.
  if (png.size() >= 24) {
    image->mutable_size()->set_width(read_be32(png.data() + 16));
    image->mutable_size()->set_height(read_be32(png.data() + 20));
  }
  image->set_uri("data:image/png;base64," + encode_base64(png.data(), png.size()));
}

// Model table structure (D3): the recognized cells carry real spans and
// header rows.  Lines bound to the table land in the first cell whose box
// contains their center; the flat cell list holds each cell once while the
// row grid repeats spanning cells across every position they cover, with
// empty unit cells filling positions no recognized cell claims.
void fill_structured_table_data(const OcrPage& page, const LayoutRegion& region,
                                pipestream::document::v1::TableData* data) {
  int rows = 0;
  int cols = 0;
  for (const auto& cell : region.structured_cells) {
    rows = std::max(rows, cell.row + cell.row_span);
    cols = std::max(cols, cell.col + cell.col_span);
  }
  data->set_num_rows(rows);
  data->set_num_cols(cols);

  struct MemberLine {
    size_t index = 0;
    AxisAlignedBox box;
  };
  std::vector<MemberLine> lines;
  for (size_t index = 0; index < page.lines.size(); ++index) {
    const auto& line = page.lines[index];
    if (line.text.empty() || line.polygon.empty()) continue;
    if (region_for_line(page, line) == &region) lines.push_back({index, bounding_box(line)});
  }
  std::ranges::sort(lines, [](const MemberLine& a, const MemberLine& b) {
    if (a.box.top != b.box.top) return a.box.top < b.box.top;
    return a.box.left < b.box.left;
  });

  std::vector<int> owner(static_cast<size_t>(rows) * static_cast<size_t>(cols), -1);
  std::vector<pipestream::document::v1::TableCell> protos;
  protos.reserve(region.structured_cells.size());
  for (const auto& cell : region.structured_cells) {
    pipestream::document::v1::TableCell proto_cell;
    proto_cell.set_row_span(cell.row_span);
    proto_cell.set_col_span(cell.col_span);
    proto_cell.set_start_row_offset_idx(cell.row);
    proto_cell.set_end_row_offset_idx(cell.row + cell.row_span);
    proto_cell.set_start_col_offset_idx(cell.col);
    proto_cell.set_end_col_offset_idx(cell.col + cell.col_span);
    proto_cell.set_column_header(cell.header);
    std::string text;
    for (const auto& member : lines) {
      const cv::Point center = member.box.center();
      const bool contains = center.x >= cell.left && center.x <= cell.right &&
                            center.y >= cell.top && center.y <= cell.bottom;
      if (!contains) continue;
      if (!text.empty()) text.push_back(' ');
      text += page.lines[member.index].text;
    }
    proto_cell.set_text(std::move(text));
    AxisAlignedBox box{cell.left, cell.top, cell.right, cell.bottom};
    set_bounding_box(box, proto_cell.mutable_bbox());
    const int cell_index = static_cast<int>(protos.size());
    for (int row = cell.row; row < cell.row + cell.row_span && row < rows; ++row) {
      for (int col = cell.col; col < cell.col + cell.col_span && col < cols; ++col) {
        auto& slot = owner[static_cast<size_t>(row) * cols + col];
        if (slot < 0) slot = cell_index;
      }
    }
    *data->add_table_cells() = proto_cell;
    protos.push_back(std::move(proto_cell));
  }
  for (int row = 0; row < rows; ++row) {
    auto* grid_row = data->add_grid();
    for (int col = 0; col < cols; ++col) {
      const int cell_index = owner[static_cast<size_t>(row) * cols + col];
      if (cell_index >= 0) {
        *grid_row->add_cells() = protos[static_cast<size_t>(cell_index)];
      } else {
        auto* blank = grid_row->add_cells();
        blank->set_row_span(1);
        blank->set_col_span(1);
        blank->set_start_row_offset_idx(row);
        blank->set_end_row_offset_idx(row + 1);
        blank->set_start_col_offset_idx(col);
        blank->set_end_col_offset_idx(col + 1);
      }
    }
  }
}

// Geometry table structure (D2 v0): every grid position becomes a TableCell
// with unit spans, mirrored into both the flat cell list and the row grid.
// Header flags stay false; geometry cannot tell a header from a body row.
void fill_table_data(const OcrPage& page, const LayoutRegion& region,
                     pipestream::document::v1::TableData* data) {
  if (!region.structured_cells.empty()) {
    fill_structured_table_data(page, region, data);
    return;
  }
  const TableGrid grid = build_table_grid(page, region);
  data->set_num_rows(grid.rows);
  data->set_num_cols(grid.cols);
  std::vector<pipestream::document::v1::TableRow*> rows;
  rows.reserve(static_cast<size_t>(grid.rows));
  for (int row = 0; row < grid.rows; ++row) rows.push_back(data->add_grid());
  for (const auto& cell : grid.cells) {
    pipestream::document::v1::TableCell proto_cell;
    proto_cell.set_row_span(1);
    proto_cell.set_col_span(1);
    proto_cell.set_start_row_offset_idx(cell.row);
    proto_cell.set_end_row_offset_idx(cell.row + 1);
    proto_cell.set_start_col_offset_idx(cell.col);
    proto_cell.set_end_col_offset_idx(cell.col + 1);
    std::string text;
    for (const size_t line_index : cell.line_indices) {
      if (!text.empty()) text.push_back(' ');
      text += page.lines[line_index].text;
    }
    proto_cell.set_text(std::move(text));
    if (!cell.line_indices.empty()) set_bounding_box(cell.box, proto_cell.mutable_bbox());
    *data->add_table_cells() = proto_cell;
    *rows[static_cast<size_t>(cell.row)]->add_cells() = std::move(proto_cell);
  }
}

}  // namespace

uint64_t utf8_codepoint_count(const std::string& text) {
  uint64_t count = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0U) != 0x80U) ++count;
  }
  return count;
}

void append_page_data(const OcrPage& source, int page_number, AssemblyCursor* cursor,
                      pipestream::parse::v1::PageData* output) {
  if (cursor == nullptr || output == nullptr) throw std::invalid_argument("Page assembly output is required");
  output->set_page_number(page_number);
  output->mutable_page_meta()->set_page_no(page_number);
  output->mutable_page_meta()->mutable_size()->set_width(source.width);
  output->mutable_page_meta()->mutable_size()->set_height(source.height);
  // The preview's pixel size comes from its own PNG header; the page size
  // above stays in the page's coordinate space (PDF points for digital
  // pages).  Same aspect ratio, so clients can scale boxes onto the image.
  if (!source.preview_png.empty()) {
    set_picture_image(source.preview_png, output->mutable_page_meta()->mutable_image());
  }

  // Emission order defines text offsets, refs, and body order, so lines are
  // walked in reading order (multi-column aware) rather than input order.
  for (const size_t line_index : reading_order(source)) {
    const auto& line = source.lines[line_index];
    if (line.text.empty() || line.polygon.empty()) continue;
    const std::string self_ref = "#/texts/" + std::to_string(cursor->text_index++);
    auto* base = output->add_texts()->mutable_text()->mutable_base();
    base->set_self_ref(self_ref);
    const LayoutRegion* region = region_for_line(source, line);
    const bool furniture = is_furniture_region(region);
    base->mutable_parent()->set_ref(furniture ? "#/furniture" : "#/body");
    base->set_content_layer(furniture ? pipestream::document::v1::CONTENT_LAYER_FURNITURE
                                      : pipestream::document::v1::CONTENT_LAYER_BODY);
    base->set_label(region == nullptr ? pipestream::document::v1::DOC_ITEM_LABEL_TEXT
                                      : label_for_region(region->label));
    base->set_orig(line.text);
    base->set_text(line.text);

    const uint64_t length = utf8_codepoint_count(line.text);
    auto* provenance = base->add_prov();
    provenance->set_page_no(page_number);
    provenance->mutable_charspan()->set_start(0);
    if (length > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      throw std::length_error("OCR line exceeds document charspan range");
    }
    provenance->mutable_charspan()->set_end(static_cast<int32_t>(length));
    set_bounding_box(bounding_box(line), provenance->mutable_bbox());

    if (cursor->has_text) ++cursor->utf_offset;
    auto* offset = output->add_text_offsets();
    offset->set_self_ref(self_ref);
    offset->set_utf_start(cursor->utf_offset);
    cursor->utf_offset += length;
    offset->set_utf_end(cursor->utf_offset);
    if (line.confidence.has_value()) offset->set_confidence(*line.confidence);
    const auto text_source = text_source_for(source, line);
    offset->set_source(text_source);
    add_collector_source(text_source == pipestream::parse::v1::TEXT_SOURCE_DIGITAL_PDF
                             ? "poppler-text"
                             : "rapidocr",
                         line.confidence, base->mutable_source());
    cursor->has_text = true;
  }

  // Table and picture regions become items in their own right so later
  // structure work has crops to work from; their inner text already streamed
  // above as TEXT.
  const std::string& layout_model = source.layout_model.empty() ? kUnnamedLayoutModel
                                                                : source.layout_model;
  for (const auto& region : source.regions) {
    if (region.label == "table") {
      auto* table = output->add_tables();
      table->set_self_ref("#/tables/" + std::to_string(cursor->table_index++));
      table->mutable_parent()->set_ref("#/body");
      table->set_content_layer(pipestream::document::v1::CONTENT_LAYER_BODY);
      table->set_label(pipestream::document::v1::DOC_ITEM_LABEL_TABLE);
      auto* provenance = table->add_prov();
      provenance->set_page_no(page_number);
      set_region_bounding_box(region, provenance->mutable_bbox());
      fill_table_data(source, region, table->mutable_data());
      add_collector_source(region.structured_cells.empty() ? "geometry" : "slanet-plus",
                           region.confidence, table->mutable_source());
    } else if (region.label == "picture") {
      auto* picture = output->add_pictures();
      picture->set_self_ref("#/pictures/" + std::to_string(cursor->picture_index++));
      picture->mutable_parent()->set_ref("#/body");
      picture->set_content_layer(pipestream::document::v1::CONTENT_LAYER_BODY);
      picture->set_label(pipestream::document::v1::DOC_ITEM_LABEL_PICTURE);
      auto* provenance = picture->add_prov();
      provenance->set_page_no(page_number);
      set_region_bounding_box(region, provenance->mutable_bbox());
      add_collector_source(layout_model, region.confidence, picture->mutable_source());
      if (!region.image_png.empty()) set_picture_image(region.image_png, picture->mutable_image());
      if (!region.figure_classes.empty()) {
        auto* classification = picture->add_annotations()->mutable_classification();
        classification->set_kind("classification");
        classification->set_provenance("figure-classifier");
        for (const auto& figure_class : region.figure_classes) {
          auto* predicted = classification->add_predicted_classes();
          predicted->set_class_name(figure_class.label);
          predicted->set_confidence(figure_class.confidence);
        }
      }
      // Decoded payloads ride as misc annotations: the upstream schema has no dedicated
      // barcode type, and the struct keeps format and value machine-readable.
      for (const auto& barcode : region.barcodes) {
        auto* misc = picture->add_annotations()->mutable_misc();
        misc->set_kind("barcode");
        auto& fields = *misc->mutable_content()->mutable_fields();
        fields["format"].set_string_value(barcode.format);
        fields["value"].set_string_value(barcode.text);
        fields["provenance"].set_string_value("zxing-cpp");
      }
    }
  }
}

void append_page_to_document(
    const OcrPage& source, int page_number, AssemblyCursor* cursor,
    pipestream::document::v1::Document* document, std::string* plain_text,
    google::protobuf::RepeatedPtrField<pipestream::parse::v1::TextOffset>* text_offsets) {
  if (document == nullptr || plain_text == nullptr) {
    throw std::invalid_argument("Document assembly output is required");
  }
  pipestream::parse::v1::PageData page;
  append_page_data(source, page_number, cursor, &page);
  if (text_offsets != nullptr) {
    for (auto& offset : *page.mutable_text_offsets()) {
      *text_offsets->Add() = std::move(offset);
    }
  }
  (*document->mutable_pages())[page_number] = std::move(*page.mutable_page_meta());

  auto* texts = page.mutable_texts();
  document->mutable_texts()->Reserve(document->texts_size() + texts->size());
  document->mutable_body()->mutable_children()->Reserve(document->body().children_size() +
                                                        texts->size());
  for (auto& text : *texts) {
    // Read everything needed from `text` before it is moved out.
    const auto& base = text.text().base();
    auto* parent = base.content_layer() == pipestream::document::v1::CONTENT_LAYER_FURNITURE
                       ? document->mutable_furniture()
                       : document->mutable_body();
    parent->add_children()->set_ref(base.self_ref());
    if (!plain_text->empty()) plain_text->push_back('\n');
    plain_text->append(base.text());
    // Hand the item over instead of deep-copying every box and string again.
    *document->add_texts() = std::move(text);
  }

  for (auto& table : *page.mutable_tables()) {
    document->mutable_body()->add_children()->set_ref(table.self_ref());
    *document->add_tables() = std::move(table);
  }
  for (auto& picture : *page.mutable_pictures()) {
    document->mutable_body()->add_children()->set_ref(picture.self_ref());
    *document->add_pictures() = std::move(picture);
  }
}

}  // namespace grparse
