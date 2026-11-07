#pragma once

#include <string>

#include "grparse/collector_coordinator.h"
#include "grparse/confluence_storage_format.h"

namespace grparse {

// The wiki storage dialect: XHTML with the macro elements of the ac: and ri:
// namespaces layered on top. It parses in process, like the CV path and
// unlike the collectors that are dialed over gRPC, because the input is a
// small well-formed XML body with a fixed construct set.

// kConfluenceStorageMimetype and confluence_storage_format(), the name
// rules, live in confluence_storage_format.h.

// Folds one storage body into a Document fragment: headings, paragraphs
// with their inline formatting and links, lists, tables, code and task
// macros, panels, and attachment pointers, every item stamped with this
// handler's CollectorSource. Never throws; a body that carries no markup at
// all becomes an INVALID_ARGUMENT outcome, and recoverable markup damage
// becomes a warning while the content already read is kept.
CollectorOutcome parse_confluence_storage(const std::string& bytes);

}  // namespace grparse
