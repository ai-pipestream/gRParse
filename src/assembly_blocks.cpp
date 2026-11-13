#include "grparse/assembly_blocks.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "grparse/list_marker.h"
#include "grparse/reading_order.h"
#include "grparse/region_geometry.h"
#include "grparse/table_structure.h"
#include "grparse/text_geometry.h"

namespace grparse {
namespace {

// Which page lines a table's own item carries, so those lines do not stream
// a second time as body prose: with model cells, every line
// assign_table_lines gives a cell; otherwise every line the geometry grid
// clustered. A line neither claims stays ordinary body text.
std::vector<bool> table_claimed_lines(const OcrPage& page, const LayoutRegion& region) {
  std::vector<bool> claimed(page.lines.size(), false);
  const TableLineAssignment assignment = assign_table_lines(page, region);
  if (assignment.structured) {
    for (size_t index = 0; index < page.lines.size(); ++index) {
      claimed[index] = assignment.cell_of_line[index] >= 0;
    }
    return claimed;
  }
  const TableGrid grid = build_table_grid(page, region);
  for (const auto& cell : grid.cells) {
    for (const size_t line_index : cell.line_indices) claimed[line_index] = true;
  }
  return claimed;
}

// Whether consecutive lines of this region merge into one item. A list
// region (a detector that boxes the whole list) stays per-line, each line
// its own item; unbound lines never merge, because nothing proves either
// belongs with its neighbor. A list_item region is the box of one item, as
// the reference reads it: its wrapped lines are one item (a bibliography
// entry over three lines is one entry), split only where a later visual
// line opens with a marker like the item's own (opens_next_item).
bool region_aggregates(const LayoutRegion* region) {
  if (region == nullptr) return false;
  return region->label != "list" && region->label != "table";
}

// Whether a list line goes on reading the visual line the block ended on:
// level with that last line (vertical overlap above half the shorter
// height) and starting right after it, within one line height. A PDF
// backend's text cells are words or segments, not lines, so without this a
// list region of the text layer turns every word into its own item; an
// item two columns away, or the next bullet below, stays its own. A block
// that so far is only its marker reaches across two line heights, the
// hanging indent between a bullet and its text, and a word that is a
// marker like the block's own ("\u2022 Red \u2022 Green", "a) foo b) bar")
// starts the next item of the same line.
bool continues_visual_line(const OcrPage& page, const TextBlock& block, const OcrLine& line) {
  const OcrLine& lead = page.lines[block.lines.front()];
  if (is_sibling_list_marker(lead.text, line.text)) return false;
  const AxisAlignedBox last = bounding_box(page.lines[block.lines.back()]);
  const AxisAlignedBox next = bounding_box(line);
  const int64_t shorter = std::min(last.height(), next.height());
  if (shorter <= 0) return false;
  const int64_t overlap = static_cast<int64_t>(std::min(last.bottom, next.bottom)) -
                          std::max(last.top, next.top);
  const int64_t gap = static_cast<int64_t>(next.left) - last.right;
  const bool only_marker = block.lines.size() == 1 && is_sibling_list_marker(lead.text, lead.text);
  const int64_t reach = (only_marker ? 2 : 1) * std::max(last.height(), next.height());
  return 2 * overlap > shorter && next.left >= last.left && gap <= reach;
}

// Whether a line inside one list_item region opens a further item: it is a
// marker like the one the block opened with ("[2]" after "[1]", a second
// bullet) and it starts a new visual line instead of going on with the
// block's last one. A detector box that swallowed two neighbouring items
// splits there; a wrapped line of one item never starts with its sibling's
// marker.
bool opens_next_item(const OcrPage& page, const TextBlock& block, const OcrLine& line) {
  const OcrLine& lead = page.lines[block.lines.front()];
  if (!is_sibling_list_marker(lead.text, line.text)) return false;
  const AxisAlignedBox last = bounding_box(page.lines[block.lines.back()]);
  const AxisAlignedBox next = bounding_box(line);
  const int64_t shorter = std::min(last.height(), next.height());
  const int64_t overlap = static_cast<int64_t>(std::min(last.bottom, next.bottom)) -
                          std::max(last.top, next.top);
  return shorter <= 0 || 2 * overlap <= shorter;
}

bool is_list_region(const LayoutRegion* region) {
  return region != nullptr && (region->label == "list" || region->label == "list_item");
}

// The hull of a block's lines.
AxisAlignedBox block_box(const OcrPage& page, const TextBlock& block) {
  AxisAlignedBox box = bounding_box(page.lines[block.lines.front()]);
  for (const size_t line_index : block.lines) {
    const AxisAlignedBox line = bounding_box(page.lines[line_index]);
    box.left = std::min(box.left, line.left);
    box.top = std::min(box.top, line.top);
    box.right = std::max(box.right, line.right);
    box.bottom = std::max(box.bottom, line.bottom);
  }
  return box;
}

bool overlaps_horizontally(const AxisAlignedBox& box, const LayoutRegion& region) {
  return std::min(box.right, region.right) - std::max(box.left, region.left) > 0;
}

bool level_and_apart(const AxisAlignedBox& a, const AxisAlignedBox& b) {
  const bool level = std::min(a.bottom, b.bottom) - std::max(a.top, b.top) > 0;
  const bool apart = std::min(a.right, b.right) - std::max(a.left, b.left) <= 0;
  return level && apart;
}

// Whether the page reads in columns around the region: some body block of
// the region's column (one it overlaps horizontally) is level with a body
// block outside that column. Furniture never counts; a running header level
// with a corner logo is not a column.
bool page_has_columns(const OcrPage& page, const std::vector<TextBlock>& blocks,
                      const LayoutRegion& region) {
  std::vector<AxisAlignedBox> own;
  std::vector<AxisAlignedBox> other;
  for (const TextBlock& block : blocks) {
    if (is_furniture_region(block.region)) continue;
    const AxisAlignedBox box = block_box(page, block);
    (overlaps_horizontally(box, region) ? own : other).push_back(box);
  }
  for (const AxisAlignedBox& a : own) {
    for (const AxisAlignedBox& b : other) {
      if (level_and_apart(a, b)) return true;
    }
  }
  return false;
}

}  // namespace

bool is_furniture_region(const LayoutRegion* region) {
  return region != nullptr && (region->label == "page_header" || region->label == "page_footer");
}

std::vector<TextBlock> build_text_blocks(const OcrPage& page) {
  // Claim maps are per table region and looked up by line below.
  std::unordered_map<const LayoutRegion*, std::vector<bool>> claims;
  for (const auto& region : page.regions) {
    if (region.label == "table") claims.emplace(&region, table_claimed_lines(page, region));
  }
  std::vector<TextBlock> blocks;
  // A trusted page (consensus vote winner) keeps its emission order; every
  // other page re-derives the order geometrically.
  for (const size_t line_index : reading_order(page, page.source_order_trusted)) {
    const auto& line = page.lines[line_index];
    if (line.text.empty() || line.polygon.empty()) continue;
    const LayoutRegion* region = region_for_line(page, line);
    bool suppressed = false;
    if (const auto claim = claims.find(region); claim != claims.end()) {
      suppressed = claim->second[line_index];
    }
    const bool same_region =
        !blocks.empty() && blocks.back().region == region && blocks.back().suppressed == suppressed;
    const bool merges =
        suppressed ||
        (region_aggregates(region) &&
         !(region->label == "list_item" && same_region && opens_next_item(page, blocks.back(), line)));
    if (same_region &&
        (merges || (is_list_region(region) && continues_visual_line(page, blocks.back(), line)))) {
      blocks.back().lines.push_back(line_index);
      continue;
    }
    TextBlock block;
    block.region = region;
    block.suppressed = suppressed;
    block.lines.push_back(line_index);
    blocks.push_back(std::move(block));
  }
  return blocks;
}

size_t region_anchor(const OcrPage& page, const std::vector<TextBlock>& blocks,
                     const LayoutRegion& region) {
  for (size_t index = 0; index < blocks.size(); ++index) {
    if (blocks[index].region == &region) return index;
  }
  // Blocks are measured by the hull of their lines: a recognizer that reads
  // word by word gives a heading a first "line" of one word at the left
  // margin, which never reaches a corner logo's column although the heading
  // does.
  const bool columns = page_has_columns(page, blocks, region);
  for (size_t index = 0; index < blocks.size(); ++index) {
    const AxisAlignedBox box = block_box(page, blocks[index]);
    if (columns && !overlaps_horizontally(box, region)) continue;
    if (box.top >= region.top) return index;
  }
  return blocks.size();
}

}  // namespace grparse
