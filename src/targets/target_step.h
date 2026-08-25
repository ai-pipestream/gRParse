// The Target step of a conversion: where the result goes once the document is
// parsed and its exports are rendered.  Targets are additive delivery, never
// a replacement, so the step only ever fills in TargetResult; the response's
// DocumentResponse is already complete and stays untouched.
#ifndef GRPARSE_TARGETS_TARGET_STEP_H
#define GRPARSE_TARGETS_TARGET_STEP_H

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"

namespace grparse::targets {

// True when `target` asks for delivery beyond the response body.  An unset
// target and an explicit InBodyTarget are the same request: nothing to do.
bool needs_delivery(const ai::pipestream::parse::v1::Target& target);

// Runs the target and fills `result`.  Blocking (it compresses, and it
// uploads), so it belongs on the same worker the conversion ran on, never on
// a gRPC event thread.  Returns UNIMPLEMENTED for the targets this server
// declares but does not serve, INVALID_ARGUMENT for a target whose own fields
// are incomplete, and UNAVAILABLE when a store refused an object.
grpc::Status deliver(const ai::pipestream::parse::v1::Target& target,
                     const ai::pipestream::document::v1::Document& document,
                     const ai::pipestream::parse::v1::DocumentExports& exports,
                     ai::pipestream::parse::v1::TargetResult* result);

}  // namespace grparse::targets

#endif
