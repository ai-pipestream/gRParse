#include "grparse/document_assembly.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

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

// A line belongs to the highest-confidence region containing its box center.
const LayoutRegion* region_for_line(const OcrPage& page, const OcrLine& line) {
  if (page.regions.empty()) return nullptr;
  const AxisAlignedBox box = bounding_box(line);
  const cv::Point center = box.center();
  const LayoutRegion* best = nullptr;
  for (const auto& region : page.regions) {
    const bool contains = center.x >= region.left && center.x <= region.right &&
                          center.y >= region.top && center.y <= region.bottom;
    if (contains && (best == nullptr || region.confidence > best->confidence)) {
      best = &region;
    }
  }
  return best;
}

void set_region_bounding_box(const LayoutRegion& region, docling::core::v1::BoundingBox* output) {
  output->set_l(region.left);
  output->set_t(region.top);
  output->set_r(region.right);
  output->set_b(region.bottom);
  output->set_coord_origin(docling::core::v1::COORD_ORIGIN_TOPLEFT);
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

  for (const auto& line : source.lines) {
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
    } else if (region.label == "figure") {
      auto* picture = output->add_pictures();
      picture->set_self_ref("#/pictures/" + std::to_string(cursor->picture_index++));
      picture->mutable_parent()->set_ref("#/body");
      picture->set_content_layer(docling::core::v1::CONTENT_LAYER_BODY);
      picture->set_label(docling::core::v1::DOC_ITEM_LABEL_PICTURE);
      auto* provenance = picture->add_prov();
      provenance->set_page_no(page_number);
      set_region_bounding_box(region, provenance->mutable_bbox());
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
