#include "grparse/document_assembly.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "grparse/base64.h"
#include "grparse/reading_order.h"
#include "grparse/region_geometry.h"
#include "grparse/table_structure.h"
#include "grparse/text_geometry.h"

namespace docling = ai::docling;

namespace grparse {
namespace {

void set_bounding_box(const AxisAlignedBox& box, docling::core::v1::BoundingBox* output) {
  output->set_l(box.left);
  output->set_t(box.top);
  output->set_r(box.right);
  output->set_b(box.bottom);
  output->set_coord_origin(docling::core::v1::COORD_ORIGIN_TOPLEFT);
}

docling::serve::v1::TextSource text_source_for(const OcrPage& page, const OcrLine& line) {
  if (line.origin.has_value()) {
    return *line.origin == TextOrigin::kDigitalPdf ? docling::serve::v1::TEXT_SOURCE_DIGITAL_PDF
                                                   : docling::serve::v1::TEXT_SOURCE_OCR;
  }
  switch (page.source) {
    case OcrPage::Source::kDigitalPdf:
      return docling::serve::v1::TEXT_SOURCE_DIGITAL_PDF;
    case OcrPage::Source::kMerged:
      // Prefer OCR label only when origin is missing; merged pages should set per-line origin.
      return docling::serve::v1::TEXT_SOURCE_OCR;
    case OcrPage::Source::kOcr:
    default:
      return docling::serve::v1::TEXT_SOURCE_OCR;
  }
}

// PubLayNet region label -> Docling item label for the text lines inside it.
// Lines inside table/figure regions keep TEXT: the region itself is emitted
// as a TableItem/PictureItem, and cell/caption structure is Epic D/E work.
docling::core::v1::DocItemLabel label_for_region(const std::string& label) {
  if (label == "title") return docling::core::v1::DOC_ITEM_LABEL_TITLE;
  if (label == "list") return docling::core::v1::DOC_ITEM_LABEL_LIST_ITEM;
  return docling::core::v1::DOC_ITEM_LABEL_TEXT;
}

void set_region_bounding_box(const LayoutRegion& region, docling::core::v1::BoundingBox* output) {
  output->set_l(region.left);
  output->set_t(region.top);
  output->set_r(region.right);
  output->set_b(region.bottom);
  output->set_coord_origin(docling::core::v1::COORD_ORIGIN_TOPLEFT);
}

// Big-endian 32-bit read for the PNG IHDR dimensions.
uint32_t read_be32(const unsigned char* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

// Embed a captured crop as a data URI.  The pixel size comes from the PNG
// IHDR itself, which is authoritative regardless of the page's coordinate
// space (digital pages measure in PDF points, crops in raster pixels).
void set_picture_image(const std::vector<unsigned char>& png, docling::core::v1::ImageRef* image) {
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
                                docling::core::v1::TableData* data) {
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
  std::sort(lines.begin(), lines.end(), [](const MemberLine& a, const MemberLine& b) {
    if (a.box.top != b.box.top) return a.box.top < b.box.top;
    return a.box.left < b.box.left;
  });

  std::vector<int> owner(static_cast<size_t>(rows) * static_cast<size_t>(cols), -1);
  std::vector<docling::core::v1::TableCell> protos;
  protos.reserve(region.structured_cells.size());
  for (const auto& cell : region.structured_cells) {
    docling::core::v1::TableCell proto_cell;
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
                     docling::core::v1::TableData* data) {
  if (!region.structured_cells.empty()) {
    fill_structured_table_data(page, region, data);
    return;
  }
  const TableGrid grid = build_table_grid(page, region);
  data->set_num_rows(grid.rows);
  data->set_num_cols(grid.cols);
  std::vector<docling::core::v1::TableRow*> rows;
  rows.reserve(static_cast<size_t>(grid.rows));
  for (int row = 0; row < grid.rows; ++row) rows.push_back(data->add_grid());
  for (const auto& cell : grid.cells) {
    docling::core::v1::TableCell proto_cell;
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
                      docling::serve::v1::PageData* output) {
  if (cursor == nullptr || output == nullptr) throw std::invalid_argument("Page assembly output is required");
  output->set_page_number(page_number);
  output->mutable_page_meta()->set_page_no(page_number);
  output->mutable_page_meta()->mutable_size()->set_width(source.width);
  output->mutable_page_meta()->mutable_size()->set_height(source.height);

  // Emission order defines text offsets, refs, and body order, so lines are
  // walked in reading order (multi-column aware) rather than input order.
  for (const size_t line_index : reading_order(source)) {
    const auto& line = source.lines[line_index];
    if (line.text.empty() || line.polygon.empty()) continue;
    const std::string self_ref = "#/texts/" + std::to_string(cursor->text_index++);
    auto* base = output->add_texts()->mutable_text()->mutable_base();
    base->set_self_ref(self_ref);
    base->mutable_parent()->set_ref("#/body");
    base->set_content_layer(docling::core::v1::CONTENT_LAYER_BODY);
    const LayoutRegion* region = region_for_line(source, line);
    base->set_label(region == nullptr ? docling::core::v1::DOC_ITEM_LABEL_TEXT
                                      : label_for_region(region->label));
    base->set_orig(line.text);
    base->set_text(line.text);

    const uint64_t length = utf8_codepoint_count(line.text);
    auto* provenance = base->add_prov();
    provenance->set_page_no(page_number);
    provenance->mutable_charspan()->set_start(0);
    if (length > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      throw std::length_error("OCR line exceeds Docling charspan range");
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
    offset->set_source(text_source_for(source, line));
    cursor->has_text = true;
  }

  // Table and figure regions become items in their own right so Epics D and E
  // have crops to work from; their inner text already streamed above as TEXT.
  for (const auto& region : source.regions) {
    if (region.label == "table") {
      auto* table = output->add_tables();
      table->set_self_ref("#/tables/" + std::to_string(cursor->table_index++));
      table->mutable_parent()->set_ref("#/body");
      table->set_content_layer(docling::core::v1::CONTENT_LAYER_BODY);
      table->set_label(docling::core::v1::DOC_ITEM_LABEL_TABLE);
      auto* provenance = table->add_prov();
      provenance->set_page_no(page_number);
      set_region_bounding_box(region, provenance->mutable_bbox());
      fill_table_data(source, region, table->mutable_data());
    } else if (region.label == "figure") {
      auto* picture = output->add_pictures();
      picture->set_self_ref("#/pictures/" + std::to_string(cursor->picture_index++));
      picture->mutable_parent()->set_ref("#/body");
      picture->set_content_layer(docling::core::v1::CONTENT_LAYER_BODY);
      picture->set_label(docling::core::v1::DOC_ITEM_LABEL_PICTURE);
      auto* provenance = picture->add_prov();
      provenance->set_page_no(page_number);
      set_region_bounding_box(region, provenance->mutable_bbox());
      if (!region.image_png.empty()) set_picture_image(region.image_png, picture->mutable_image());
    }
  }
}

void append_page_to_document(const OcrPage& source, int page_number, AssemblyCursor* cursor,
                             docling::core::v1::DoclingDocument* document, std::string* plain_text) {
  if (document == nullptr || plain_text == nullptr) {
    throw std::invalid_argument("Document assembly output is required");
  }
  docling::serve::v1::PageData page;
  append_page_data(source, page_number, cursor, &page);
  (*document->mutable_pages())[page_number] = std::move(*page.mutable_page_meta());

  auto* texts = page.mutable_texts();
  document->mutable_texts()->Reserve(document->texts_size() + texts->size());
  document->mutable_body()->mutable_children()->Reserve(document->body().children_size() +
                                                        texts->size());
  for (auto& text : *texts) {
    // Read everything needed from `text` before it is moved out.
    const auto& base = text.text().base();
    document->mutable_body()->add_children()->set_ref(base.self_ref());
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
