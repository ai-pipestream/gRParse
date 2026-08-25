#pragma once

#include <cstdint>

#include "ai/pipestream/parse/v1/parse_stream.pb.h"
#include "grparse/ocr_types.h"

namespace grparse {

struct AssemblyCursor {
  uint64_t text_index = 0;
  uint64_t table_index = 0;
  uint64_t picture_index = 0;
  uint64_t utf_offset = 0;
  bool has_text = false;
};

uint64_t utf8_codepoint_count(const std::string& text);
void append_page_data(const OcrPage& source, int page_number, AssemblyCursor* cursor,
                      ai::pipestream::parse::v1::PageData* output);
// Folds one page into the document. `text_offsets`, when given, collects the
// page's offset rows: the same side table the streaming surface puts on the
// wire, kept for the unary callers that need to locate an item in the
// document's concatenated text stream. Passing nullptr discards them exactly
// as before.
void append_page_to_document(
    const OcrPage& source, int page_number, AssemblyCursor* cursor,
    ai::pipestream::document::v1::Document* document, std::string* plain_text,
    google::protobuf::RepeatedPtrField<ai::pipestream::parse::v1::TextOffset>* text_offsets =
        nullptr);

}  // namespace grparse
