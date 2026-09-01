#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

// Splitting the items a text-layer fold ran together. A collector that
// builds paragraphs from lines cannot see that a numbered all-caps heading
// and the sentence after it are two things, or that each checkbox row of a
// form is its own paragraph. Both are visible in the text alone, so the
// repair pass splits them here, for documents from geometry collectors
// only: a structural producer's paragraphs are its own.

// The offset where a run-in heading ends: the text opens with decimal
// numbering ("3.1"), then at least two all-caps words, then a word in
// sentence case that starts the paragraph proper. Absent otherwise.
std::optional<size_t> run_in_heading_end(std::string_view text);

// The offsets (beyond zero) where form rows begin inside one item: a
// checkbox glyph (U+2610, U+2612) after whitespace, or a capitalized label
// ending in a colon that a run of underscores follows ("Signature: ____").
std::vector<size_t> form_row_starts(std::string_view text);

// Splits the text item at `arena_index` at the given byte offsets: the item
// keeps the first piece, each further piece becomes a new TEXT item of the
// same layer, parent and sources appended to the texts arena and inserted
// after the item in its parent's children. Each provenance box is cut into
// vertical strips proportional to the pieces' character shares, so every
// piece's box lies inside the original's; spans follow the piece that holds
// their start. Returns the new items' references in order.
std::vector<std::string> split_text_item(ai::pipestream::document::v1::Document* document,
                                         int arena_index, const std::vector<size_t>& offsets);

// Every body prose item from the named collectors that opens with a run-in
// heading becomes a SECTION_HEADER holding the heading and a new TEXT item
// holding the rest. Returns the number of headings split out.
int split_run_in_headings(ai::pipestream::document::v1::Document* document,
                          const std::vector<std::string>& geometry_collectors);

// Every body prose item from the named collectors that holds several form
// rows becomes one item per row. Returns the number of rows split off.
int split_form_rows(ai::pipestream::document::v1::Document* document,
                    const std::vector<std::string>& geometry_collectors);

}  // namespace grparse
