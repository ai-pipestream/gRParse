#include "grparse/document_assembly.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace docling = ai::docling;

namespace grparse {
namespace {

void set_bounding_box(const OcrLine& line, docling::core::v1::BoundingBox* box) {
  int left = std::numeric_limits<int>::max();
  int top = std::numeric_limits<int>::max();
  int right = std::numeric_limits<int>::min();
  int bottom = std::numeric_limits<int>::min();
  for (const auto& point : line.polygon) {
    left = std::min(left, point.x);
    top = std::min(top, point.y);
    right = std::max(right, point.x);
    bottom = std::max(bottom, point.y);
  }
  box->set_l(left);
  box->set_t(top);
  box->set_r(right);
  box->set_b(bottom);
  box->set_coord_origin(docling::core::v1::COORD_ORIGIN_TOPLEFT);
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
    base->set_label(docling::core::v1::DOC_ITEM_LABEL_TEXT);
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
    set_bounding_box(line, provenance->mutable_bbox());

    if (cursor->has_text) ++cursor->utf_offset;
    auto* offset = output->add_text_offsets();
    offset->set_self_ref(self_ref);
    offset->set_utf_start(cursor->utf_offset);
    cursor->utf_offset += length;
    offset->set_utf_end(cursor->utf_offset);
    if (line.confidence.has_value()) offset->set_confidence(*line.confidence);
    offset->set_source(source.source == OcrPage::Source::kDigitalPdf
                           ? docling::serve::v1::TEXT_SOURCE_DIGITAL_PDF
                           : docling::serve::v1::TEXT_SOURCE_OCR);
    cursor->has_text = true;
  }
}

void append_page_to_document(const OcrPage& source, int page_number, AssemblyCursor* cursor,
                             docling::core::v1::DoclingDocument* document, std::string* plain_text) {
  if (document == nullptr || plain_text == nullptr) {
    throw std::invalid_argument("Document assembly output is required");
  }
  docling::serve::v1::PageData page;
  append_page_data(source, page_number, cursor, &page);
  (*document->mutable_pages())[page_number].CopyFrom(page.page_meta());
  for (const auto& text : page.texts()) {
    const auto& base = text.text().base();
    document->mutable_body()->add_children()->set_ref(base.self_ref());
    document->add_texts()->CopyFrom(text);
    if (!plain_text->empty()) plain_text->push_back('\n');
    plain_text->append(base.text());
  }
}

}  // namespace grparse
