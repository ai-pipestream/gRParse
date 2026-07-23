#pragma once

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

// Additively merges `source` into `target`: source's arena items append
// with renumbered self_refs so references stay unique, every reference
// inside the moved items is rewritten to match, body and furniture children
// and metadata append, and pages merge by page number with target winning a
// collision. Nothing already in target is modified, which is the
// scatter-gather rule: sources never overwrite each other.
void merge_documents(ai::pipestream::document::v1::Document&& source,
                     ai::pipestream::document::v1::Document* target);

}  // namespace grparse
