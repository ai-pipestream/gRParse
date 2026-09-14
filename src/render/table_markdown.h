// The Markdown table formatter: the model's computed cell grid rendered as a
// padded GFM table. Internal to the export renderers;
// include/grparse/document_render.h stays the only public surface.
#ifndef GRPARSE_RENDER_TABLE_MARKDOWN_H
#define GRPARSE_RENDER_TABLE_MARKDOWN_H

#include <cstddef>
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

// The number of leading grid rows that form the column header, the way the
// reference resolves a spanned header to the one row GFM allows: rows on
// which a column_header cell starts; 1 when no cell is flagged at all (the
// first row stays the header); 0 when flags exist but none starts on row 0
// (every row is body).
std::size_t count_header_rows(
    const std::vector<std::vector<const ai::pipestream::document::v1::TableCell*>>& grid);

// The reference's padded table layout: one leading and one trailing space
// per cell, a column width of at least the header width plus two, data
// cells stripped and padded to the column width, and a dashed rule under
// the header row. Stacked header rows flatten per column, joined with
// " - " with consecutive duplicates (a row-spanning cell) dropped. With
// `compact` the padding goes: every cell stripped, the rule one dash per
// column (docling-core's compact_tables).
std::string table_markdown(const ai::pipestream::document::v1::TableData& data,
                           const CellTextResolver& resolve_ref, bool compact = false);

}  // namespace grparse::render

#endif
