#pragma once

// What every collector client in this directory shares: the upload and
// drain halves of a bidirectional collector stream, the status mapping, and
// the deadline ceilings. One file per collector holds the wire specifics;
// nothing here knows which collector it is serving.
//
// Internal to src/collectors: the public surface stays
// include/grparse/document_collectors.h.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "grparse/collector_coordinator.h"

namespace grparse {

// Upload chunk size, matching the office collector: small enough to
// interleave with response reads, large enough to keep the frame count low.
inline constexpr size_t kChunkBytes = 256U * 1024U;

// A hung collector must not pin the parse forever. ASR gets a longer leash:
// transcription runs at a fraction of media real time, not of byte count.
// These are ceilings, not the deadline: capped_collector_deadline pulls each
// leg in to the inbound call's own deadline whenever the caller passed one.
inline constexpr std::chrono::minutes kDeadline{5};
inline constexpr std::chrono::minutes kAsrDeadline{30};

// The collector's own status classes survive where they are meaningful to
// the caller; transport-level failures collapse to UNAVAILABLE.
grpc::StatusCode map_code(grpc::StatusCode code);

// The shared tail of every client: turn stream results into an outcome.
// `document_seen` distinguishes "collector too old to know emit_document"
// from a healthy stream; both trailer and document are hard requirements.
CollectorOutcome finish_outcome(const char* name, const grpc::Status& status,
                                bool trailer_seen, bool document_seen,
                                CollectorOutcome outcome);

// Writes the options request already staged in `request`, then the payload
// in kChunkBytes frames, then half-closes. `fill_chunk(frame, offset,
// length, last)` stages one payload frame in place; `always_send_chunk`
// forces one frame even for an empty payload, for wires whose final frame
// carries a completion marker. A failed Write ends the upload early; the
// reason surfaces through the stream's Finish status.
template <typename Stream, typename Request, typename FillChunk>
void upload_stream(Stream& stream, Request& request, const std::string& bytes,
                   bool always_send_chunk, FillChunk fill_chunk) {
  if (!stream.Write(request)) return;
  size_t offset = 0;
  bool chunk_sent = false;
  while (offset < bytes.size() || (always_send_chunk && !chunk_sent)) {
    const size_t length = std::min(kChunkBytes, bytes.size() - offset);
    request.Clear();
    fill_chunk(request, offset, length, offset + length >= bytes.size());
    offset += length;
    chunk_sent = true;
    if (!stream.Write(request)) return;
  }
  stream.WritesDone();
}

// Drains the response stream into an outcome. The document event is the
// payload; the collector's typed events are dropped, because the fold
// already happened where the events were made. `on_status(event, warnings)`
// returns true when the event is the terminal status, appending any
// warnings that status carries.
template <typename Response, typename Stream, typename OnStatus>
CollectorOutcome drain_stream(const char* name, Stream& stream, OnStatus on_status) {
  CollectorOutcome outcome;
  bool trailer_seen = false;
  bool document_seen = false;
  Response event;
  while (stream.Read(&event)) {
    if (event.has_document()) {
      outcome.document = std::move(*event.mutable_document());
      document_seen = true;
    } else if (on_status(event, outcome.warnings)) {
      trailer_seen = true;
    }
    event.Clear();
  }
  return finish_outcome(name, stream.Finish(), trailer_seen, document_seen,
                        std::move(outcome));
}

}  // namespace grparse
