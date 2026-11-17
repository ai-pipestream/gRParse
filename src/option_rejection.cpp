#include "option_rejection.h"

#include "google/rpc/status.pb.h"

namespace grparse {

grpc::Status option_rejected(const std::string& option_path,
                             ai::pipestream::parse::v1::OptionRejectionReason reason,
                             const std::string& rejected_value_raw, const std::string& message) {
  ai::pipestream::parse::v1::OptionRejection rejection;
  rejection.set_option_path(option_path);
  rejection.set_reason(reason);
  rejection.set_rejected_value_raw(rejected_value_raw);
  google::rpc::Status status;
  status.set_code(static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT));
  status.set_message(message);
  if (!status.add_details()->PackFrom(rejection)) {
    // Packing a generated message cannot fail; if it ever does the people
    // message still stands, without the typed detail.
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, message);
  }
  return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, message, status.SerializeAsString());
}

}  // namespace grparse
