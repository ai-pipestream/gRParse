// Internal seams of the Docs API export (src/render/gdocs_renderer.cpp): the
// named style a heading level maps to, the spelling of a list identifier, and
// the text-run folding every paragraph goes through. Declared here so the
// export's tests can pin them directly; not part of the public API, where
// include/grparse/document_render.h stays the only surface.
#ifndef GRPARSE_RENDER_GDOCS_RENDERER_H
#define GRPARSE_RENDER_GDOCS_RENDERER_H

#include <cstddef>
#include <string>
#include <string_view>

namespace grparse::render {

// The named paragraph style a section header of `level` maps to: "HEADING_1"
// through "HEADING_6", with anything below the first rank lifted to it and
// anything past the last clamped down. Unlike the HTML export, which reserves
// <h1> for the document title, this export has the API's own TITLE style for
// that, so the first heading rank stays free for level 1.
std::string gdocs_heading_style(int level);

// The identifier of the `index`-th list in the export. Lists are numbered in
// the order the walk meets them, which is what keeps two renders of the same
// document byte-identical.
std::string gdocs_list_id(std::size_t index);

// The content of a paragraph's single text run: the item text with carriage
// returns dropped and every remaining newline folded to the vertical tab the
// API reads as a soft line break, then the trailing newline that terminates a
// paragraph's run.
std::string gdocs_run_content(std::string_view text);

}  // namespace grparse::render

#endif
