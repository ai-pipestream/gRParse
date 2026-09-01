#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_assembly.h"

namespace grparse {

// Heading depth over a whole document. Three signals, in order of trust:
// numbering in the heading text ("1", "1.1", "A.", "IV", "Appendix B"),
// the all-caps section words a paper uses without numbers (ABSTRACT,
// REFERENCES), and the heading's size relative to the numbered ones. A
// document that numbers nothing falls back to clustering sizes alone, the
// way the CV path always did. A title (the heading block that opens the
// first page, taller than every other heading) is level 1 and pushes every
// section one level down.

struct HeadingOptions {
  // Collectors whose heading levels are guesses from geometry, re-derived
  // here over the whole document; any other producer's nonzero level wins.
  std::vector<std::string> geometry_collectors{"pdf"};
  // Fold consecutive title lines on the first page into one item.
  bool merge_title_lines = true;
};

struct HeadingReport {
  // Title lines folded into their predecessor.
  int titles_merged = 0;
  // Section headers that became the document's TitleItem.
  int titles_promoted = 0;
  // Section headers whose level was set or changed.
  int levels_assigned = 0;
  // Headers from geometry collectors relabelled as prose: a quoted line, a
  // sentence, or a paragraph a font heuristic called a heading.
  int headings_demoted = 0;
};

// True when a heading's text reads as prose rather than a heading: it
// opens with a quotation mark, ends with sentence punctuation or a closing
// quotation mark, or runs past thirty words.
bool heading_reads_as_prose(std::string_view text);

// The depth a heading's numbering states: "1" and "A." and "IV" and
// "Appendix A" are 1, "1.1" and "A.1" are 2, "1.1.1" is 3, and so on.
// Absent for an unnumbered heading and for a bare number with no text
// after it (a page number).
std::optional<int> heading_numbering_depth(std::string_view text);

// True for an unnumbered heading of one to three all-caps words
// (ABSTRACT, RELATED WORK, ACKNOWLEDGMENTS): a peer of the numbered
// top-level sections.
bool is_section_word_heading(std::string_view text);

// Levels for the headings, see the module comment: numbered depth is the
// level, unnumbered headings take the nearest numbered cluster's, a
// document without numbering clusters heights; title lines (see
// title_lines) report level 1 here and become the TitleItem on a Document.
// Heights compare within one unit only: declared font sizes when every
// heading has one, else the median line heights, as before.
std::map<std::string, int32_t> infer_heading_levels(std::vector<HeaderHeight> headers);

// The self_refs of the headings that form the document's title, in reading
// order: the heading block that opens the first page (lines of similar
// height, each starting within a line height of the one before, none
// numbered) when at least one other heading exists and every other heading
// is visibly smaller than the block's tallest line (below 85%). Empty when
// the first page's opening heading is no taller than the rest.
std::vector<std::string> title_lines(const std::vector<HeaderHeight>& headers);

// Runs the whole inference on a Document. Eligible section headers are
// those with level zero or produced only by `options.geometry_collectors`.
// From geometry collectors, a header that reads as prose becomes a TEXT
// item. Consecutive title lines that are also consecutive body children
// merge into the first (text joined with a space, provenance and spans
// carried, the rest retired with every reference renumbered), and the
// title becomes the document's TitleItem, unless the document already has
// one. The remaining eligible headers take their level from
// infer_heading_levels. Idempotent.
HeadingReport infer_heading_hierarchy(ai::pipestream::document::v1::Document* document,
                                      const HeadingOptions& options = {});

}  // namespace grparse
