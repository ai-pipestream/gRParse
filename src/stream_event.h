#pragma once

#include <memory>
#include <utility>

#include "ai/pipestream/parse/v1/parse_stream.pb.h"

namespace grparse::detail {

// One queued streaming write. Keep its message on the heap, like incoming
// collector Documents: an event-owned arena would turn their moves into
// deep copies held until the write completes.
struct StreamEvent {
  ai::pipestream::parse::v1::DocumentStreamEvent message;
  // CV page writes return scheduler credit; collector pages never took one.
  bool scheduler_credit = false;
};

// Internal production construction boundary: transfer a collector Document
// into the actual queued event without copying its picture buffers or items.
inline std::unique_ptr<StreamEvent> make_collector_document_event(
    ai::pipestream::document::v1::Document&& document) {
  auto event = std::make_unique<StreamEvent>();
  *event->message.mutable_collector_document()->mutable_document() = std::move(document);
  return event;
}

}  // namespace grparse::detail
