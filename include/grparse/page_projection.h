#pragma once

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
// furniture tree, then any arena item neither tree reaches. An item's page
// is its first page-numbered provenance; an item without one rides the page
// of the item before it (page 1 at the start). Page metadata comes from
// Document.pages when the collector filled it, otherwise only the number.
//
// A document that places nothing on a page projects to no pages at all, and
// the caller keeps its single collector-document event; page numbering in
// the result is dense from 1 to the highest page the document names.
std::vector<ai::pipestream::parse::v1::PageData> project_page_data(
    const ai::pipestream::document::v1::Document& document,
    ai::pipestream::parse::v1::TextSource text_source);

}  // namespace grparse
