#pragma once

#include <string>

#include <grpcpp/support/status.h>

#include "ai/pipestream/parse/v1/parse_types.pb.h"

namespace grparse {

// An INVALID_ARGUMENT that names the rejected option: the message for
// people, and a typed OptionRejection packed in google.rpc.Status.details
// (the status's error_details) for programs.
grpc::Status option_rejected(const std::string& option_path,
                             ai::pipestream::parse::v1::OptionRejectionReason reason,
                             const std::string& rejected_value_raw, const std::string& message);

}  // namespace grparse
