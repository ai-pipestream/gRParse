#pragma once

#include <cstddef>
#include <vector>

#include "grparse/ocr_types.h"

namespace grparse {

// One body block: a run of consecutive reading-order lines bound to the same
// layout region, or a single unbound line. The block is the unit of item
// emission: a prose region becomes one item whose provenance keeps every
// member line's box and charspan, instead of one item per OCR line.
struct TextBlock {
  const LayoutRegion* region = nullptr;
  std::vector<size_t> lines;
  // The text of these lines already rides inside the region's own item (a
  // table's cells), so the block anchors ordering but emits nothing.
  bool suppressed = false;
};

// Running headers and footers are page furniture, not body prose: they carry
// the furniture content layer and hang off the furniture group instead of
// #/body, so renderers that walk the body never fold a page number into the
// running text.
bool is_furniture_region(const LayoutRegion* region);

// The page's blocks in reading order (reading_order.h): consecutive lines of
// one aggregating region join a block, lines a table's own item carries are
// marked suppressed, and a trusted page (consensus vote winner) keeps its
// emission order.
std::vector<TextBlock> build_text_blocks(const OcrPage& page);

// Where a floating region (a table, a picture) belongs in the block
// sequence: the index of the block it is emitted before, blocks.size() for
// after everything on the page.
//
// Blocks are measured by the hull of their lines (a word-by-word recognizer
// makes a heading's first line one word at the margin).
//
// 1. Before the first block it owns lines of.
// 2. Else before the first block in its own column (one it overlaps
//    horizontally) that starts below its top edge. Reading order is
//    column-major, so a float with text beside it belongs to its column and
//    comparing tops across columns would anchor a right-column float into
//    the middle of the left one.
// 3. Else, when no body block runs beside it (level with it in another
//    column), before the first block anywhere that starts below its top
//    edge: nothing sits beside the float, so it heads a band of its own (a
//    logo in the top corner above a narrower title, a signature beside no
//    text) rather than closing a column it was never part of.
// 4. Else after everything on the page: the float ends its column.
size_t region_anchor(const OcrPage& page, const std::vector<TextBlock>& blocks,
                     const LayoutRegion& region);

}  // namespace grparse
