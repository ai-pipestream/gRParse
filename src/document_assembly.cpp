#include "grparse/document_assembly.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "render/renderer_base.h"
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

// One body block: a run of consecutive reading-order lines bound to the same
// layout region, or a single unbound line. The block is the unit of item
// emission: a prose region becomes one item whose provenance keeps every
// member line's box and charspan, instead of one item per OCR line.
struct TextBlock {
  const LayoutRegion* region = nullptr;
  std::vector<size_t> lines;
  // The text of these lines already rides inside the region's own item (a
  // table's cells), so the block anchors ordering but emits nothing.
  bool suppressed = false;
};

// Which page lines a table's own item carries, so those lines do not stream
// a second time as body prose. Structured cells claim a line when its center
// falls inside a recognized cell; the geometry grid claims every line it
// clustered. A line neither claims stays ordinary body text.
std::vector<bool> table_claimed_lines(const OcrPage& page, const LayoutRegion& region) {
  std::vector<bool> claimed(page.lines.size(), false);
  if (!region.structured_cells.empty()) {
    for (size_t index = 0; index < page.lines.size(); ++index) {
      const auto& line = page.lines[index];
      if (line.text.empty() || line.polygon.empty()) continue;
      if (region_for_line(page, line) != &region) continue;
      const cv::Point center = bounding_box(line).center();
      for (const auto& cell : region.structured_cells) {
        if (center.x >= cell.left && center.x <= cell.right && center.y >= cell.top &&
            center.y <= cell.bottom) {
          claimed[index] = true;
          break;
        }
      }
    }
    return claimed;
  }
  const TableGrid grid = build_table_grid(page, region);
  for (const auto& cell : grid.cells) {
    for (const size_t line_index : cell.line_indices) claimed[line_index] = true;
  }
  return claimed;
}

// Whether consecutive lines of this region merge into one item. Lists stay
// per-line (each line is its own item) and unbound lines never merge,
// because nothing proves either belongs with its neighbor.
bool region_aggregates(const LayoutRegion* region) {
  if (region == nullptr) return false;
  return region->label != "list" && region->label != "list_item" && region->label != "table";
}

std::vector<TextBlock> build_text_blocks(const OcrPage& page) {
  // Claim maps are per table region and looked up by line below.
  std::unordered_map<const LayoutRegion*, std::vector<bool>> claims;
  for (const auto& region : page.regions) {
    if (region.label == "table") claims.emplace(&region, table_claimed_lines(page, region));
  }
  std::vector<TextBlock> blocks;
  for (const size_t line_index : reading_order(page)) {
    const auto& line = page.lines[line_index];
    if (line.text.empty() || line.polygon.empty()) continue;
    const LayoutRegion* region = region_for_line(page, line);
    bool suppressed = false;
    if (const auto claim = claims.find(region); claim != claims.end()) {
      suppressed = claim->second[line_index];
    }
    const bool merges = suppressed || region_aggregates(region);
    if (merges && !blocks.empty() && blocks.back().region == region &&
        blocks.back().suppressed == suppressed) {
      blocks.back().lines.push_back(line_index);
      continue;
    }
    TextBlock block;
    block.region = region;
    block.suppressed = suppressed;
    block.lines.push_back(line_index);
    blocks.push_back(std::move(block));
  }
  return blocks;
}

// True when every vertex of the quad lies on a corner of its own hull, so
// the polygon carries no information the box does not.
bool polygon_is_axis_aligned(const std::vector<cv::Point>& polygon, const AxisAlignedBox& box) {
  for (const auto& vertex : polygon) {
    const bool on_x = vertex.x == box.left || vertex.x == box.right;
    const bool on_y = vertex.y == box.top || vertex.y == box.bottom;
    if (!on_x || !on_y) return false;
  }
  return true;
}

// Where a floating region belongs in the block sequence: before the first
// block it owns lines of, else before the first block that starts below its
// top edge, else after everything on the page.
size_t region_anchor(const OcrPage& page, const std::vector<TextBlock>& blocks,
                     const LayoutRegion& region) {
  for (size_t index = 0; index < blocks.size(); ++index) {
    if (blocks[index].region == &region) return index;
  }
  for (size_t index = 0; index < blocks.size(); ++index) {
    const auto& first = page.lines[blocks[index].lines.front()];
    if (bounding_box(first).top >= region.top) return index;
  }
  return blocks.size();
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

  // Emission order defines text offsets, refs, and body order, so blocks are
  // walked in reading order (multi-column aware) rather than input order,
  // with floating items placed at their reading-order anchors instead of
  // appended after the page's prose.
  const std::vector<TextBlock> blocks = build_text_blocks(source);

  struct Placed {
    const LayoutRegion* region;
    size_t anchor;
  };
  std::vector<Placed> placed;
  for (const auto& region : source.regions) {
    if (region.label != "table" && region.label != "picture") continue;
    placed.push_back({&region, region_anchor(source, blocks, region)});
  }
  std::ranges::stable_sort(placed, [](const Placed& a, const Placed& b) {
    if (a.anchor != b.anchor) return a.anchor < b.anchor;
    if (a.region->top != b.region->top) return a.region->top < b.region->top;
    return a.region->left < b.region->left;
  });

  const std::string& layout_model = source.layout_model.empty() ? kUnnamedLayoutModel
                                                                : source.layout_model;

  // Floats and caption items emitted on this page, kept for the caption
  // attachment pass; body_order collects the page's body children in the
  // order a reader meets them.
  struct EmittedFloat {
    const LayoutRegion* region;
    std::string self_ref;
    google::protobuf::RepeatedPtrField<pipestream::document::v1::RefItem>* captions;
  };
  std::vector<EmittedFloat> floats;
  struct EmittedCaption {
    const LayoutRegion* region;
    std::string self_ref;
    pipestream::document::v1::TextItemBase* base;
  };
  std::vector<EmittedCaption> captions;
  std::vector<std::string> body_order;

  const auto emit_float = [&](const LayoutRegion& region) {
    if (region.label == "table") {
      auto* table = output->add_tables();
      const std::string self_ref = "#/tables/" + std::to_string(cursor->table_index++);
      table->set_self_ref(self_ref);
      table->mutable_parent()->set_ref("#/body");
      table->set_content_layer(pipestream::document::v1::CONTENT_LAYER_BODY);
      table->set_label(pipestream::document::v1::DOC_ITEM_LABEL_TABLE);
      auto* provenance = table->add_prov();
      provenance->set_page_no(page_number);
      set_region_bounding_box(region, provenance->mutable_bbox());
      fill_table_data(source, region, table->mutable_data());
      add_collector_source(region.structured_cells.empty() ? "geometry" : "slanet-plus",
                           region.confidence, table->mutable_source());
      floats.push_back({&region, self_ref, table->mutable_captions()});
      body_order.push_back(self_ref);
      return;
    }
    auto* picture = output->add_pictures();
    const std::string self_ref = "#/pictures/" + std::to_string(cursor->picture_index++);
    picture->set_self_ref(self_ref);
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
    // Decoded payloads ride on the typed barcode arm, and once more as the
    // legacy misc-annotation struct so existing consumers keep working for
    // one release.
    for (const auto& barcode : region.barcodes) {
      auto* typed = picture->add_annotations()->mutable_barcode();
      typed->set_format(barcode.format);
      typed->set_value(barcode.text);
      typed->set_provenance("zxing-cpp");
      auto* misc = picture->add_annotations()->mutable_misc();
      misc->set_kind("barcode");
      auto& fields = *misc->mutable_content()->mutable_fields();
      fields["format"].set_string_value(barcode.format);
      fields["value"].set_string_value(barcode.text);
      fields["provenance"].set_string_value("zxing-cpp");
    }
    floats.push_back({&region, self_ref, picture->mutable_captions()});
    body_order.push_back(self_ref);
  };

  const auto emit_block = [&](const TextBlock& block) {
    const LayoutRegion* region = block.region;
    // Code lines keep their line structure; prose members join with spaces.
    const char separator = region != nullptr && region->label == "code" ? '\n' : ' ';
    std::string merged;
    struct MemberSpan {
      size_t line;
      uint64_t start;
      uint64_t end;
    };
    std::vector<MemberSpan> spans;
    spans.reserve(block.lines.size());
    uint64_t code_points = 0;
    for (const size_t line_index : block.lines) {
      const auto& line = source.lines[line_index];
      if (!merged.empty()) {
        merged.push_back(separator);
        ++code_points;
      }
      const uint64_t length = utf8_codepoint_count(line.text);
      spans.push_back({line_index, code_points, code_points + length});
      code_points += length;
      merged += line.text;
    }
    if (code_points > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      throw std::length_error("Text block exceeds document charspan range");
    }

    const std::string self_ref = "#/texts/" + std::to_string(cursor->text_index++);
    const auto label = region == nullptr ? pipestream::document::v1::DOC_ITEM_LABEL_TEXT
                                         : label_for_region(region->label);
    auto* item = output->add_texts();
    pipestream::document::v1::TextItemBase* base = nullptr;
    if (label == pipestream::document::v1::DOC_ITEM_LABEL_TITLE) {
      base = item->mutable_title()->mutable_base();
    } else if (label == pipestream::document::v1::DOC_ITEM_LABEL_SECTION_HEADER) {
      // Level stays unset here; assign_section_header_levels clusters the
      // whole document's heading heights once every page is in.
      base = item->mutable_section_header()->mutable_base();
    } else {
      base = item->mutable_text()->mutable_base();
    }
    const bool furniture = is_furniture_region(region);
    base->set_self_ref(self_ref);
    base->mutable_parent()->set_ref(furniture ? "#/furniture" : "#/body");
    base->set_content_layer(furniture ? pipestream::document::v1::CONTENT_LAYER_FURNITURE
                                      : pipestream::document::v1::CONTENT_LAYER_BODY);
    base->set_label(label);
    base->set_orig(merged);
    base->set_text(merged);
    // One provenance entry per member line: its own box, its own charspan
    // into the merged text (code points), so nothing about where each line
    // sat on the page is lost to the merge.
    for (const auto& span : spans) {
      const auto& member = source.lines[span.line];
      auto* provenance = base->add_prov();
      provenance->set_page_no(page_number);
      provenance->mutable_charspan()->set_start(static_cast<int32_t>(span.start));
      provenance->mutable_charspan()->set_end(static_cast<int32_t>(span.end));
      const AxisAlignedBox box = bounding_box(member);
      set_bounding_box(box, provenance->mutable_bbox());
      // Rotated or skewed lines keep their exact quad; an axis-aligned quad
      // adds nothing over the box and is skipped.
      if (!polygon_is_axis_aligned(member.polygon, box)) {
        for (const auto& vertex : member.polygon) {
          auto* point = provenance->add_polygon();
          point->set_x(vertex.x);
          point->set_y(vertex.y);
        }
      }
    }

    if (cursor->has_text) ++cursor->utf_offset;
    auto* offset = output->add_text_offsets();
    offset->set_self_ref(self_ref);
    offset->set_utf_start(cursor->utf_offset);
    cursor->utf_offset += code_points;
    offset->set_utf_end(cursor->utf_offset);
    cursor->has_text = true;

    // The row's confidence is the weakest member's; a block is only as
    // trustworthy as its shakiest line.
    std::optional<float> confidence;
    bool digital = false;
    bool ocr = false;
    for (const size_t line_index : block.lines) {
      const auto& line = source.lines[line_index];
      if (line.confidence.has_value()) {
        confidence = confidence.has_value() ? std::min(*confidence, *line.confidence)
                                            : *line.confidence;
      }
      if (text_source_for(source, line) == pipestream::parse::v1::TEXT_SOURCE_DIGITAL_PDF) {
        digital = true;
      } else {
        ocr = true;
      }
    }
    if (confidence.has_value()) offset->set_confidence(*confidence);
    // A uniform block keeps its origin; mixed digital and OCR members report
    // OCR, the weaker claim.
    offset->set_source(digital && !ocr ? pipestream::parse::v1::TEXT_SOURCE_DIGITAL_PDF
                                       : pipestream::parse::v1::TEXT_SOURCE_OCR);
    if (digital) add_collector_source("poppler-text", confidence, base->mutable_source());
    if (ocr) add_collector_source("rapidocr", confidence, base->mutable_source());

    if (region != nullptr && region->label == "caption") captions.push_back({region, self_ref, base});
    if (!furniture) body_order.push_back(self_ref);
  };

  size_t next_placed = 0;
  for (size_t index = 0; index <= blocks.size(); ++index) {
    while (next_placed < placed.size() && placed[next_placed].anchor == index) {
      emit_float(*placed[next_placed].region);
      ++next_placed;
    }
    if (index == blocks.size()) break;
    if (!blocks[index].suppressed) emit_block(blocks[index]);
  }

  // A caption binds to the nearest table or picture it visually labels: at
  // least 30% of the caption's width overlapping horizontally and a vertical
  // gap of at most 1.5 caption heights, nearest gap wins. The claimed
  // caption re-parents under the float and leaves body order; renderers then
  // emit it with its float instead of as free prose.
  for (auto& caption : captions) {
    const double width = caption.region->right - caption.region->left;
    const double height = caption.region->bottom - caption.region->top;
    if (width <= 0 || height <= 0) continue;
    const EmittedFloat* best = nullptr;
    double best_gap = 0;
    for (const auto& target : floats) {
      const double overlap = std::min(caption.region->right, target.region->right) -
                             std::max(caption.region->left, target.region->left);
      if (overlap < 0.3 * width) continue;
      double gap = 0;
      if (caption.region->top >= target.region->bottom) {
        gap = caption.region->top - target.region->bottom;
      } else if (caption.region->bottom <= target.region->top) {
        gap = target.region->top - caption.region->bottom;
      }
      if (gap > 1.5 * height) continue;
      if (best == nullptr || gap < best_gap) {
        best = &target;
        best_gap = gap;
      }
    }
    if (best == nullptr) continue;
    caption.base->mutable_parent()->set_ref(best->self_ref);
    best->captions->Add()->set_ref(caption.self_ref);
    std::erase(body_order, caption.self_ref);
  }

  for (const auto& ref : body_order) output->add_body_order()->set_ref(ref);
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

  // With a body order the page names its own body children (captions a
  // float claimed and furniture rows already absent); without one, legacy
  // producers fall back to texts-tables-pictures order.
  const bool ordered = page.body_order_size() > 0;

  auto* texts = page.mutable_texts();
  document->mutable_texts()->Reserve(document->texts_size() + texts->size());
  document->mutable_body()->mutable_children()->Reserve(document->body().children_size() +
                                                        texts->size());
  for (auto& text : *texts) {
    // Read everything needed from `text` before it is moved out.
    const auto* base = render::text_base(text);
    if (base != nullptr) {
      const bool furniture =
          base->content_layer() == pipestream::document::v1::CONTENT_LAYER_FURNITURE;
      if (furniture) {
        document->mutable_furniture()->add_children()->set_ref(base->self_ref());
      } else if (!ordered) {
        document->mutable_body()->add_children()->set_ref(base->self_ref());
      }
      if (!plain_text->empty()) plain_text->push_back('\n');
      plain_text->append(base->text());
    }
    // Hand the item over instead of deep-copying every box and string again.
    *document->add_texts() = std::move(text);
  }

  for (auto& table : *page.mutable_tables()) {
    if (!ordered) document->mutable_body()->add_children()->set_ref(table.self_ref());
    *document->add_tables() = std::move(table);
  }
  for (auto& picture : *page.mutable_pictures()) {
    if (!ordered) document->mutable_body()->add_children()->set_ref(picture.self_ref());
    *document->add_pictures() = std::move(picture);
  }
  for (const auto& ref : page.body_order()) {
    document->mutable_body()->add_children()->set_ref(ref.ref());
  }
}

void assign_section_header_levels(pipestream::document::v1::Document* document) {
  if (document == nullptr) throw std::invalid_argument("Document is required");
  struct Pending {
    pipestream::document::v1::SectionHeaderItem* item;
    double height;
  };
  std::vector<Pending> pending;
  for (auto& text : *document->mutable_texts()) {
    if (text.item_case() != pipestream::document::v1::BaseTextItem::kSectionHeader) continue;
    auto* header = text.mutable_section_header();
    if (header->level() > 0) continue;  // the producer already chose
    std::vector<double> heights;
    for (const auto& provenance : header->base().prov()) {
      const auto& box = provenance.bbox();
      const double height = std::abs(box.b() - box.t());
      if (height > 0) heights.push_back(height);
    }
    if (heights.empty()) {
      header->set_level(1);
      continue;
    }
    // The median line height stands for the heading; one clipped or merged
    // line must not drag a heading into another cluster.
    const auto middle = heights.begin() + static_cast<std::ptrdiff_t>(heights.size() / 2);
    std::nth_element(heights.begin(), middle, heights.end());
    pending.push_back({header, *middle});
  }
  if (pending.empty()) return;
  // Tallest first; a heading founds a deeper level when it is visibly
  // smaller (below 85%) than the current level's founding height. Depth
  // saturates at 6, the deepest level exports render.
  std::vector<Pending*> by_height;
  by_height.reserve(pending.size());
  for (auto& entry : pending) by_height.push_back(&entry);
  std::ranges::stable_sort(by_height,
                           [](const Pending* a, const Pending* b) { return a->height > b->height; });
  int level = 0;
  double founding = std::numeric_limits<double>::infinity();
  for (auto* entry : by_height) {
    if (entry->height < 0.85 * founding) {
      level = std::min(level + 1, 6);
      founding = entry->height;
    }
    entry->item->set_level(std::max(level, 1));
  }
}

}  // namespace grparse
