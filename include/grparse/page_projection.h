#pragma once

#include <cstdint>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.pb.h"

namespace grparse {

// Projects a collector's Document onto the streaming surface's page events,
// so a parse that arrives as one finished document (the pdf inspector's
// fast path, any collector that places its items on pages) reaches stream
// consumers in the same shape the in-process CV pipeline emits: one PageData
// per page, items in reading order, text offsets and body order included.
//
// Reading order is the body tree walk (groups recursed) followed by the
// furniture tree, then any arena item neither tree reaches. Each group
// rides the page of the first item placed under it; a group with none, or
// one no tree reaches, rides the page of the last item placed. An item's
// page is its first page-numbered provenance; an item without one rides the page
// of the item before it (page 1 at the start). Page metadata comes from
// Document.pages when the collector filled it, otherwise only the number.
//
// A document that places nothing on a page projects to no pages at all, and
// the caller keeps its single collector-document event. The result holds the
// pages the document names (an item placed on it, or a Document.pages
// entry), in page order; a page nothing names is not emitted, so a single
// bogus page number costs one page rather than a run of empty ones up to it.
//
// `utf_offset`, when given, is where the document's text stream stands
// before this document's first item, and is advanced past its last: a
// caller projecting a document page by page (the inspector's page slices)
// keeps one stream of offsets across the calls. Null starts at zero.
std::vector<ai::pipestream::parse::v1::PageData> project_page_data(
    const ai::pipestream::document::v1::Document& document,
    ai::pipestream::parse::v1::TextSource text_source, uint64_t* utf_offset = nullptr);

}  // namespace grparse
