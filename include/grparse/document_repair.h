#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

// The post-merge repair pass. Every collector folds what it can see: one
// page, one region, one line. Some of what a reader considers wrong with a
// parse is only visible once the whole Document exists: the running header
// that repeats on every page, the word a line break cut in two, the
// paragraph a page break split. This module fixes those three on the
// finished Document, format-agnostically, as pure functions a caller can
// run one at a time or all at once through repair_document.

struct RepairOptions {
  bool demote_running_furniture = true;
  bool rejoin_hyphenation = true;
  bool merge_continuations = true;
  // A body item is running furniture when its normalized text recurs on at
  // least this many distinct pages and on at least this share of the
  // document's pages (the larger of the two applies).
  int minimum_repeat_pages = 3;
  double minimum_repeat_share = 0.4;
  // The top and bottom bands of a page, as a fraction of its height, where
  // furniture lives.
  double band_fraction = 0.12;
  // Continuation merges per pass; a run of short unpunctuated lines (a
  // poem, an address block) stops here instead of folding into one item.
  int maximum_continuation_merges = 256;
  // Print one stdout line per document the pass changed.
  bool log_report = false;
};

struct RepairReport {
  // Body items relabelled as page header or footer and moved to furniture.
  int furniture_demoted = 0;
  // The normalized strings that recurred often enough to be furniture, plus
  // "<page number>" once when standalone page numbers were demoted.
  std::vector<std::string> furniture_patterns;
  // Words rejoined across a line-break hyphen inside one item.
  int hyphens_rejoined = 0;
  int soft_hyphens_removed = 0;
  // Body paragraphs merged into their predecessor across a page or column
  // break.
  int paragraphs_merged = 0;

  // Whether the arenas or any item's text changed: everything but a
  // demotion, which only relabels and re-parents. A side table that
  // describes item text by reference (an offset table) is stale when this
  // is true.
  bool changed_text_or_arenas() const {
    return hyphens_rejoined > 0 || soft_hyphens_removed > 0 || paragraphs_merged > 0;
  }
  bool changed_anything() const { return furniture_demoted > 0 || changed_text_or_arenas(); }
};

// Runs the enabled repairs in their fixed order: furniture demotion first
// (so paragraphs a page break split become body neighbours), continuation
// merging second, hyphenation rejoin last. Idempotent: a second run over
// the result changes nothing.
RepairReport repair_document(ai::pipestream::document::v1::Document* document,
                             const RepairOptions& options = {});

// Repair 1. On a multi-page document, a body text item whose normalized
// text recurs across enough pages, sitting in the top or bottom band of its
// page, is a running header or footer: it is relabelled PAGE_HEADER or
// PAGE_FOOTER, given the furniture content layer, and its reference moves
// from the body tree to the furniture tree in page order. A standalone page
// number in a band (see is_page_number) counts even though it recurs on no
// other page. Section headers, titles, and anything inside a
// group (a list, a table's captions, a chapter) are never demoted. Only
// items with a page and a box are candidates. Returns the count; the
// matched patterns append to `patterns` when given.
int demote_running_furniture(ai::pipestream::document::v1::Document* document,
                             const RepairOptions& options,
                             std::vector<std::string>* patterns = nullptr);

// The normalization repair 1 compares by: ASCII case folded, whitespace
// runs collapsed to one space, digit runs replaced by '#', trimmed.
std::string normalize_running_text(std::string_view text);

// True when an item's text is nothing but a page number in one of the
// usual dressings ("3", "- 3 -", "Page 3 of 12", "iv") whose value is
// plausible for a document of `page_count` pages: within [1, page_count +
// 20], never a year unless the document has that many pages, and never
// followed by sentence punctuation (a wrapped reference line ending
// "2023." is prose), except an item that is exactly a number and one
// period.
bool is_page_number(std::string_view text, int page_count);

struct HyphenationCounts {
  int rejoined = 0;
  int soft_hyphens_removed = 0;
};

// Repair 2, on one string: a lowercase letter, a hyphen, a line break (or
// the single space a line join left), and a lowercase letter become the
// joined word when both fragments are alphabetic and the pair is not a
// known hyphenated compound ("self-", "well-", "non-" always; "pre-",
// "post-", "co-", "re-" before a vowel). Soft hyphens (U+00AD) are removed
// everywhere. Counts accumulate into `counts` when given.
std::string rejoin_hyphenated_words(std::string_view text, HyphenationCounts* counts = nullptr);

// Repair 2 over every TEXT or PARAGRAPH item of the document.
HyphenationCounts rejoin_hyphenation(ai::pipestream::document::v1::Document* document);

// The word two fragments make when a hyphen sat between them at a line
// end: kept hyphenated for a known compound, concatenated otherwise.
std::string join_hyphenated_fragments(std::string_view head, std::string_view tail);

// Repair 3. A TEXT or PARAGRAPH body item that ends without terminal
// punctuation (or with a hyphen), whose next body sibling is a TEXT or
// PARAGRAPH starting with a lowercase letter, absorbs that sibling when the
// layout explains the split: the sibling sits on the next page and the
// item had run into the lower half of its own page, or the sibling sits on
// the same page across a column break (starting at least a line above the
// item's last line, wholly to its right, with a real word first). Texts join
// with a space (or by the hyphen rule), provenance, sources, spans and
// comments carry over, and the sibling is retired from the body and the
// texts arena with every reference renumbered. Only direct body children
// merge, so a section header, a list, a table or any group between two
// items keeps them apart. Returns the number of merges, capped by the
// options.
int merge_continuations(ai::pipestream::document::v1::Document* document,
                        const RepairOptions& options);

// Process-wide totals of what the pass changed since startup, for the
// metrics exposition beside the pipeline counters.
struct RepairTotals {
  uint64_t furniture_demoted = 0;
  uint64_t hyphens_rejoined = 0;
  uint64_t paragraphs_merged = 0;
};

// The pass as the service runs it: repair_document, the report added to
// the process-wide totals, and one stdout line when the options ask for it.
RepairReport run_repair_pass(ai::pipestream::document::v1::Document* document,
                             const RepairOptions& options);

RepairTotals repair_totals();

}  // namespace grparse
