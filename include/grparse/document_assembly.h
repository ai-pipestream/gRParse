#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

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
// Embeds a PNG as a data URI on the image ref, sized from its own header.
void set_picture_image(const std::vector<unsigned char>& png,
                       ai::pipestream::document::v1::ImageRef* image);
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

// One section header awaiting a depth: its reference, its median line
// height in page pixels, and its median declared font size in points when
// the text layer states one (zero otherwise).
struct HeaderHeight {
  std::string self_ref;
  double height = 0;
  double font_size = 0;
};

// Clusters heading heights into depths: the tallest cluster is level 1,
// each visibly smaller cluster (below 85% of its predecessor's founding
// height) one level deeper, saturating at 6. An entry with no usable
// height takes level 1. Heights are only comparable when every page
// rasterized at the same scale, which is how the CV path renders.
std::map<std::string, int32_t> section_header_levels(std::vector<HeaderHeight> headers);

// The median prov box heights of a page's level-less section headers, in
// the shape section_header_levels consumes. The streaming surface collects
// these per page and ships the clustered result with its terminal event.
void collect_header_heights(const ai::pipestream::parse::v1::PageData& page,
                            std::vector<HeaderHeight>* into);

// Assigns heading levels to section headers the detector produced, by
// clustering their line heights across the whole document. Items whose
// producer already chose a level (anything nonzero) are left alone.
void assign_section_header_levels(ai::pipestream::document::v1::Document* document);

}  // namespace grparse
