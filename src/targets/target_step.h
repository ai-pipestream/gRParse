// The Target step of a conversion: where the result goes once the document is
// parsed and its exports are rendered.  Targets are additive delivery, never
// a replacement, so the step only ever fills in TargetResult; the response's
// DocumentResponse is already complete and stays untouched.
#ifndef GRPARSE_TARGETS_TARGET_STEP_H
#define GRPARSE_TARGETS_TARGET_STEP_H

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "s3_client.h"

#include <optional>
#include <string>

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

// The credentials an S3 request signs with: the request's own pair (both or
// neither), else, only when the deployment opted in, the server's ambient
// AWS_* identity (GRPARSE_S3_AMBIENT_CREDENTIALS=1, an https endpoint listed
// in GRPARSE_S3_AMBIENT_ENDPOINTS, and a verified peer). Shared by S3Target
// and S3Source; the refusals start with config->label.
grpc::Status resolve_s3_credentials(const std::optional<std::string>& access_key,
                                    const std::optional<std::string>& secret_key,
                                    const std::string& endpoint, S3Config* config);

}  // namespace grparse::targets

#endif
