#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

// The post-merge repair pass. Every collector folds what it can see: one
// page, one region, one line. Some of what a reader considers wrong with a
// parse is only visible once the whole Document exists: the running header
// that repeats on every page, the word a line break cut in two, the
// paragraph a page break split, the heading whose depth only makes sense
// beside every other heading, the page whose floats a text-layer fold put
// first. This module fixes those on the finished Document,
// format-agnostically, as pure functions a caller can run one at a time or
// all at once through repair_document.

struct RepairOptions {
  bool demote_running_furniture = true;
  bool rejoin_hyphenation = true;
  bool merge_continuations = true;
  // Heading hierarchy (heading_hierarchy.h): title lines merged, levels
  // from numbering and size, for headers without a level and for those
  // from `geometry_collectors`.
  bool infer_heading_hierarchy = true;
  // Body order (document_reading_order.h): each page's direct body
  // children in XY-cut reading order, only when the whole document came
  // from `geometry_collectors`.
  bool order_body_by_geometry = true;
  // Paragraph splits (paragraph_split.h) for items from
  // `geometry_collectors`: a numbered all-caps heading run into its
  // paragraph, and form rows folded into one item.
  bool split_paragraphs = true;
  // Collectors whose order and heading levels are guesses from geometry
  // rather than document structure; every other producer's choices win.
  std::vector<std::string> geometry_collectors{"pdf"};
  // Collectors that build a prose item by joining its lines with one
  // space, so a space after a hyphen may be where a line broke: the pdf
  // text layer, grparse's own OCR and layout assembly, and the VLM convert
  // leg's transcriptions. Every other producer's spaces are text.
  std::vector<std::string> line_joined_collectors{"pdf", "grparse", "vlm-convert"};
  // A body item is running furniture when its normalized text recurs on at
  // least this many distinct pages and on at least this share of the
  // document's pages (the larger of the two applies).
  int minimum_repeat_pages = 3;
  double minimum_repeat_share = 0.4;
  // The top and bottom bands of a page, as a fraction of its height, where
  // furniture lives.
  double band_fraction = 0.12;
  // A running header or footer is a line or two; a longer item in a band
  // is a paragraph however often the document repeats it.
  int maximum_furniture_words = 12;
  // Continuation merges per pass; a run of short unpunctuated lines (a
  // poem, an address block) stops here instead of folding into one item.
  int maximum_continuation_merges = 256;
  // Print one stdout line per document the pass changed.
  bool log_report = false;

  // The structural repairs docling-core #810 adds (structure_repair.cpp).
  // All off by default, so the pass output is unchanged unless a caller
  // asks; ConvertDocumentOptions.structure_repairs turns them on per
  // request. They run after every repair above, in this order.
  //
  // Move the furniture tree's children into the body with the furniture
  // content layer. While on, the furniture demotion and the continuation
  // merge look past furniture-layer body items, as they did when those
  // items sat in the furniture tree, so the migration changes where the
  // references live and nothing else, and a second pass is a no-op.
  bool migrate_furniture_tree = false;
  // List an orphaned caption, footnote or reference under the floating
  // item that names it and that it names as its parent.
  bool repair_referenced_orphans = false;
  // Wrap each list group child that is not a list item in a new list item.
  bool wrap_list_children = false;
  // Remove non-root groups without children that nothing claims as parent.
  bool remove_empty_groups = false;
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
  // Title lines on the first page folded into the first of them, and the
  // section header that became the document's TitleItem.
  int titles_merged = 0;
  int titles_promoted = 0;
  // Section headers whose level was set or changed, and geometry
  // collectors' headers relabelled as prose.
  int heading_levels_assigned = 0;
  int headings_demoted = 0;
  // Run-in headings split out of the paragraph that followed them, and
  // form rows split out of one item (paragraph_split.h).
  int headings_split = 0;
  int form_rows_split = 0;
  // Direct body children whose position changed, and the pages that held
  // them.
  int body_items_reordered = 0;
  int pages_reordered = 0;
  // The structural repairs: furniture tree children moved into the body,
  // orphans listed by their parent, list children wrapped in a new list
  // item, empty groups removed.
  int furniture_tree_migrated = 0;
  int orphans_repaired = 0;
  int list_children_wrapped = 0;
  int empty_groups_removed = 0;

  // Whether the arenas or any item's text changed: everything but a
  // demotion, a level, or a body order change, which only relabel and
  // re-parent. A side table that describes item text by reference (an
  // offset table) is stale when this is true.
  bool changed_text_or_arenas() const {
    return hyphens_rejoined > 0 || soft_hyphens_removed > 0 || paragraphs_merged > 0 ||
           titles_merged > 0 || headings_split > 0 || form_rows_split > 0 ||
           list_children_wrapped > 0 || empty_groups_removed > 0;
  }
  bool changed_anything() const {
    return furniture_demoted > 0 || heading_levels_assigned > 0 || body_items_reordered > 0 ||
           titles_promoted > 0 || headings_demoted > 0 || furniture_tree_migrated > 0 ||
           orphans_repaired > 0 || changed_text_or_arenas();
  }
};

// Runs the enabled repairs in their fixed order: furniture demotion first
// (so paragraphs a page break split become body neighbours), the run-in
// heading and form row splits, heading hierarchy, body order (so
// continuations meet their real neighbours), continuation merging,
// hyphenation rejoin, then the opt-in structural repairs (furniture tree
// migration, referenced orphans, list children, empty groups), and the
// body order once more when the migration moved furniture into a body the
// order pass owns. Idempotent: a second run over the result changes
// nothing.
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

// A run of a text the hyphen rejoin removed, half-open, in code points of
// the text it was given.
struct RemovedRun {
  size_t start = 0;
  size_t end = 0;
};

// Repair 2, on one string: a lowercase letter, a hyphen, a line break and a
// lowercase letter become the joined word when both fragments are
// alphabetic and the pair is not a known hyphenated compound ("self-",
// "well-", "non-" always; "pre-", "post-", "co-", "re-" before a vowel).
// A line break is a newline; the single space a line join left counts as
// one only when `space_is_break` (text a collector joined from lines), so "short- and" in authored prose is never a break. The tail's
// first token must be a word of two letters or more, letters only up to a
// trailing punctuation mark: "hyper-" followed by a stray "t" (a subscript
// line folded into the paragraph) or by "x2" is not a broken word and
// keeps its hyphen. A suspended hyphen keeps its hyphen too: a tail token
// that is a conjunction ("and", "or", "to", "und", ...) followed by a
// hyphenated word ("short-\nand long-term") or, after a German conjunction,
// a capitalized one ("Vor-\nund Nachteile"). A tail that only spells a
// conjunction ("thous-\nand people", "tick-\net office") rejoins. Soft
// hyphens (U+00AD) are removed everywhere. Counts accumulate into `counts`
// and the removed runs, in order, append to `removed` when given.
std::string rejoin_hyphenated_words(std::string_view text, HyphenationCounts* counts = nullptr,
                                    bool space_is_break = false,
                                    std::vector<RemovedRun>* removed = nullptr);

// Repair 2 over every TEXT or PARAGRAPH item of the document, group
// members included. A single space is a line break only in items produced
// by `line_joined_collectors`. Inline span ranges and provenance charspans
// move with the text past every removed run.
HyphenationCounts rejoin_hyphenation(ai::pipestream::document::v1::Document* document,
                                     const std::vector<std::string>& line_joined_collectors = {});

// The word two fragments make when a hyphen sat between them at a line
// end: kept hyphenated for a known compound, concatenated otherwise.
std::string join_hyphenated_fragments(std::string_view head, std::string_view tail);

// Repair 3. A TEXT or PARAGRAPH body item that ends without terminal
// punctuation (or with a hyphen), whose next body sibling is a TEXT or
// PARAGRAPH starting with a lowercase letter, absorbs that sibling when the
// layout explains the split: the sibling sits on the next page and the
// item had run into the lower half of its own page, or the sibling sits on
// the same page across a column break (starting at least a line above the
// item's last line, wholly to its right). Either way the sibling's first
// token must be a word of two letters or more (a lone letter is an
// enumerator), and a short sibling (under 6 words) ending in terminal
// punctuation after a long item (12 words or more) is a caption-like
// fragment, not a continuation. Texts join
// with a space (or by the hyphen rule, a suspended hyphen keeping its
// hyphen and the space), provenance, sources, spans and
// comments carry over, and the sibling is retired from the body and the
// texts arena with every reference renumbered. Only direct body children
// merge, so a section header, a list, a table or any group between two
// items keeps them apart. Returns the number of merges, capped by the
// options.
int merge_continuations(ai::pipestream::document::v1::Document* document,
                        const RepairOptions& options);

// Removes the retired text items (the keys of `absorbed_by`, each mapped
// to the item that absorbed it) from the texts arena, renumbers what
// remains, prunes them from every group's children and from the children
// of the item (table, picture, text) that is a retired item's parent,
// hands a retired item's own children to its survivor,
// and points every reference anywhere in the Document at its new name:
// orphans no parent lists and graph cells' item_refs included. A reference
// into a retired item follows it to the item that absorbed it. Shared by
// every repair that folds two items into one.
void retire_text_items(ai::pipestream::document::v1::Document* document,
                       const std::map<std::string, std::string>& absorbed_by);

// The structural repairs, each usable alone. Every one is idempotent: a
// second call on its own result returns 0 and changes nothing.
//
// Moves the deprecated furniture tree's children into the body
// (docling-core _migrate_furniture_to_body): each moved item and its
// descendants on the body layer take the furniture layer, and its parent
// becomes #/body. A header (PAGE_HEADER, or a box centred in the upper
// half of its page) goes before the first body item on its page, a footer
// after the last; within a page they keep docling-core's visual order
// (12-point line band, left edge, source order). Unlocated headers lead
// the body and unlocated footers trail it. When no body item names a page
// the placement is docling-core's: every header before the body, every
// footer after it. Returns the number of furniture children moved.
int migrate_furniture_tree(ai::pipestream::document::v1::Document* document);

// Lists each caption, footnote or reference a floating item (picture,
// table, code, key-value or form item) names, and whose parent is that
// item, among the item's children when it is missing there
// (docling-core _repair_referenced_orphans). Returns the number added.
int repair_referenced_orphans(ai::pipestream::document::v1::Document* document);

// Wraps each child of a LIST or ORDERED_LIST group that is not a list item,
// and whose parent is that group, in a new empty ListItem at the same
// position (docling-core _migrate_non_list_item_list_children); the child
// keeps its arena slot and becomes the new item's only child. The new item
// is appended to the texts arena, takes the child's content layer, and is
// enumerated like its list item siblings (or, with none, when the group is
// ORDERED_LIST). Returns the number wrapped.
int wrap_list_children(ai::pipestream::document::v1::Document* document);

// Removes every group other than the roots that has a parent, no children,
// and that no item names as its parent, repeating until none is left
// (docling-core _remove_empty_groups). The groups arena is renumbered with
// every reference rewritten; a remaining reference into a removed group
// (an anchor, a span target) follows it to its parent. Returns the number
// removed.
int remove_empty_groups(ai::pipestream::document::v1::Document* document);

// Process-wide totals of what the pass changed since startup, for the
// metrics exposition beside the pipeline counters.
struct RepairTotals {
  uint64_t furniture_demoted = 0;
  uint64_t hyphens_rejoined = 0;
  uint64_t paragraphs_merged = 0;
  uint64_t titles_merged = 0;
  uint64_t heading_levels_assigned = 0;
  uint64_t body_items_reordered = 0;
  uint64_t headings_split = 0;
  uint64_t headings_demoted = 0;
  uint64_t form_rows_split = 0;
  uint64_t furniture_tree_migrated = 0;
  uint64_t orphans_repaired = 0;
  uint64_t list_children_wrapped = 0;
  uint64_t empty_groups_removed = 0;
};

// The pass as the service runs it: repair_document, the report added to
// the process-wide totals, and one stdout line when the options ask for it.
RepairReport run_repair_pass(ai::pipestream::document::v1::Document* document,
                             const RepairOptions& options);

RepairTotals repair_totals();

}  // namespace grparse
