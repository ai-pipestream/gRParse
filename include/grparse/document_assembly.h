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
void append_page_to_document(const OcrPage& source, int page_number, AssemblyCursor* cursor,
                             ai::pipestream::document::v1::Document* document, std::string* plain_text);

}  // namespace grparse
