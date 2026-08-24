// Terminal display width of UTF-8 text, as the reference table formatter
// measures it when padding Markdown table columns. Not part of the public
// API; include/grparse/document_render.h stays the only public surface.
#ifndef GRPARSE_RENDER_DISPLAY_WIDTH_H
#define GRPARSE_RENDER_DISPLAY_WIDTH_H

#include <string>
#include <string_view>

namespace grparse::render {

// Display width in terminal cells: 0 for combining and other zero-width
// code points, 2 for East Asian Wide and Fullwidth ones, 1 otherwise, and
// -1 for the whole string when it contains a C0/C1 control character (the
// reference measurement propagates that sentinel). Pure ASCII printable
// text measures as its length, which is the only path real table cells
// take. Grapheme-cluster effects the reference applies on top of this
// (emoji joined by ZWJ, variation selectors, regional indicator pairs,
// Indic conjuncts) are deliberately not modelled: they change only the
// column padding of a cell that carries such a cluster.
int display_width(std::string_view text);

}  // namespace grparse::render

#endif
