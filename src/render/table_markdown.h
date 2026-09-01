// The Markdown table formatter: the model's computed cell grid rendered as a
// padded GFM table. Internal to the export renderers;
// include/grparse/document_render.h stays the only public surface.
#ifndef GRPARSE_RENDER_TABLE_MARKDOWN_H
#define GRPARSE_RENDER_TABLE_MARKDOWN_H

#include <functional>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse::render {

// How a cell that points at another item renders. The renderer supplies it,
// because resolving a reference means serializing whatever it names; a cell
// that carries its text inline never reaches it.
using CellTextResolver = std::function<std::string(const std::string& ref)>;

// The cell text of the model's computed grid, with the two characters a
// Markdown row cannot carry rewritten.
std::vector<std::vector<std::string>> table_rows(
    const ai::pipestream::document::v1::TableData& data,
    const CellTextResolver& resolve_ref);

// The reference's padded table layout: one leading and one trailing space
// per cell, a column width of at least the header width plus two, data
// cells stripped and padded to the column width, and a dashed rule under
// the header row.
std::string table_markdown(const ai::pipestream::document::v1::TableData& data,
                           const CellTextResolver& resolve_ref);

}  // namespace grparse::render

#endif
