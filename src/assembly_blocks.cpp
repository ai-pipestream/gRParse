#include "grparse/assembly_blocks.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "grparse/reading_order.h"
#include "grparse/region_geometry.h"
#include "grparse/table_structure.h"
#include "grparse/text_geometry.h"

namespace grparse {
namespace {

// Which page lines a table's own item carries, so those lines do not stream
// a second time as body prose. Structured cells claim a line when its center
// falls inside a recognized cell; the geometry grid claims every line it
// clustered. A line neither claims stays ordinary body text.
std::vector<bool> table_claimed_lines(const OcrPage& page, const LayoutRegion& region) {
  std::vector<bool> claimed(page.lines.size(), false);
  if (!region.structured_cells.empty()) {
    for (size_t index = 0; index < page.lines.size(); ++index) {
      const auto& line = page.lines[index];
      if (line.text.empty() || line.polygon.empty()) continue;
      if (region_for_line(page, line) != &region) continue;
      const cv::Point center = bounding_box(line).center();
      for (const auto& cell : region.structured_cells) {
        if (center.x >= cell.left && center.x <= cell.right && center.y >= cell.top &&
            center.y <= cell.bottom) {
          claimed[index] = true;
          break;
        }
      }
    }
    return claimed;
  }
  const TableGrid grid = build_table_grid(page, region);
  for (const auto& cell : grid.cells) {
    for (const size_t line_index : cell.line_indices) claimed[line_index] = true;
  }
  return claimed;
}

// Whether consecutive lines of this region merge into one item. Lists stay
// per-line (each line is its own item) and unbound lines never merge,
// because nothing proves either belongs with its neighbor.
bool region_aggregates(const LayoutRegion* region) {
  if (region == nullptr) return false;
  return region->label != "list" && region->label != "list_item" && region->label != "table";
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

// Whether a body block runs beside the region: level with it (vertical
// overlap) in another column (no horizontal overlap). Furniture never
// counts; a running header level with a corner logo is not a column.
bool block_runs_beside(const OcrPage& page, const TextBlock& block, const LayoutRegion& region) {
  if (is_furniture_region(block.region)) return false;
  const AxisAlignedBox box = block_box(page, block);
  if (overlaps_horizontally(box, region)) return false;
  return std::min(box.bottom, region.bottom) - std::max(box.top, region.top) > 0;
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
    const bool merges = suppressed || region_aggregates(region);
    if (merges && !blocks.empty() && blocks.back().region == region &&
        blocks.back().suppressed == suppressed) {
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
  for (size_t index = 0; index < blocks.size(); ++index) {
    const AxisAlignedBox box = bounding_box(page.lines[blocks[index].lines.front()]);
    if (!overlaps_horizontally(box, region)) continue;
    if (box.top >= region.top) return index;
  }
  const bool in_a_column = std::ranges::any_of(blocks, [&](const TextBlock& block) {
    return block_runs_beside(page, block, region);
  });
  if (in_a_column) return blocks.size();
  for (size_t index = 0; index < blocks.size(); ++index) {
    if (bounding_box(page.lines[blocks[index].lines.front()]).top >= region.top) return index;
  }
  return blocks.size();
}

}  // namespace grparse
