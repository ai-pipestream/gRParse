// Which nodes of a parsed storage tree are blocks. Internal to the storage
// handler; include/grparse/confluence_storage.h stays the only public
// surface.
#ifndef GRPARSE_CONFLUENCE_STORAGE_BLOCKS_H
#define GRPARSE_CONFLUENCE_STORAGE_BLOCKS_H

#include "storage_node.h"

namespace grparse::confluence {

// True for a node that is a block in its own right, or a container that
// turns out to hold one.
bool is_block(const Node& node);

// True when a block element wraps another block: such a paragraph cannot be
// read as a run of inline text without losing what it wraps.
bool contains_block(const Node& node);

}  // namespace grparse::confluence

#endif
