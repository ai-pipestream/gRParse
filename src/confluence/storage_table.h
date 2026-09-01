// The HTML table geometry the storage fold places cells with: row discovery,
// span attributes, slot occupancy and the materialized grid. Internal to the
// storage handler; include/grparse/confluence_storage.h stays the only public
// surface.
#ifndef GRPARSE_CONFLUENCE_STORAGE_TABLE_H
#define GRPARSE_CONFLUENCE_STORAGE_TABLE_H

#include <string_view>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "storage_node.h"

namespace grparse::confluence {

// The grid ceiling the office fold uses: a declared row/column product above
// it keeps the placed cells and skips the materialized grid, so a bogus
// header can never allocate an arbitrary rectangle.
inline constexpr int kMaxGridCells = 4096;

// The column ceiling bounds the placement walk itself, so a body full of wide
// column spans cannot grow the row occupancy without limit.
inline constexpr int kMaxColumns = 4096;

// One row of a parsed table and whether it sits in the header section.
struct TableRowNode {
  const Node* row = nullptr;
  bool head = false;
};

// Every "tr" of the subtree in document order, with the header section
// flagged; section wrappers are transparent.
void collect_rows(const Node& node, bool head, std::vector<TableRowNode>* rows);

// A "rowspan" or "colspan" value. Spans are clamped to the table's own
// extent; anything larger is malformed markup, and the clamp keeps offsets
// inside the grid it describes.
int span_attribute(const Node& cell, std::string_view name, bool* clamped);

// Marks every slot a cell covers as taken, growing the rows it reaches.
void reserve_slots(std::vector<std::vector<bool>>* occupied, int row, int column,
                   int row_span, int col_span);

// The materialized grid: a spanning cell fills every slot it covers, and
// nothing writes outside the rectangle the header declared.
void materialize_grid(ai::pipestream::document::v1::TableData* data, int num_rows,
                      int num_cols);

}  // namespace grparse::confluence

#endif
