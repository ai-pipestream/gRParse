// The Markdown post-processing primitives the renderer applies to item text:
// stripping, escaping, line-break rules, humanizing and joining. Internal to
// the export renderers; include/grparse/document_render.h stays the only
// public surface.
#ifndef GRPARSE_RENDER_MARKDOWN_TEXT_H
#define GRPARSE_RENDER_MARKDOWN_TEXT_H

#include <string>
#include <string_view>
#include <vector>

namespace grparse::render {

// Whitespace-stripped copy, matching the strip the table formatter applies to
// every data cell.
std::string stripped(std::string_view text);

// Underscore escaping: every "_" not already escaped becomes "\_", except
// inside an inline image target, which is left verbatim so URLs survive. The
// image pattern is "![" alt "](" target ")" with neither part crossing a
// newline, matched left to right without overlap.
std::string escape_underscores(const std::string& text);

// GFM hard line breaks: a lone newline gains two trailing spaces, a blank
// line stays a paragraph break.
std::string md_line_breaks(const std::string& text);

// Headings cannot span lines, so their newlines collapse to spaces.
std::string heading_line_breaks(std::string text);

// The reference's humanizer: underscores become spaces and the first
// character is upper-cased (ASCII; the vocabularies it runs on are ASCII).
std::string humanized(std::string text);

// The non-empty parts, in order, separated by `sep`.
std::string join(const std::vector<std::string>& parts, std::string_view sep);

}  // namespace grparse::render

#endif
