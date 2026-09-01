#include "collector_support.h"

#include <string>
#include <utility>

#include "grparse/document_collectors.h"

namespace grparse {

grpc::StatusCode map_code(grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::INVALID_ARGUMENT:
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
    case grpc::StatusCode::DEADLINE_EXCEEDED:
    case grpc::StatusCode::UNIMPLEMENTED:
      return code;
    default:
      return grpc::StatusCode::UNAVAILABLE;
  }
}

CollectorOutcome finish_outcome(const char* name, const grpc::Status& status,
                                bool trailer_seen, bool document_seen,
                                CollectorOutcome outcome) {
  if (!status.ok()) {
    outcome.error = collector_status_text(name, status);
    outcome.code = map_code(status.error_code());
    outcome.success = false;
    return outcome;
  }
  if (!trailer_seen) {
    outcome.error = std::string(name) + " collector: stream ended without a terminal status";
    outcome.code = grpc::StatusCode::UNAVAILABLE;
    outcome.success = false;
    return outcome;
  }
  if (!document_seen) {
    outcome.error = std::string(name) +
                    " collector: stream ended without a document event; the "
                    "collector predates emit_document";
    outcome.code = grpc::StatusCode::UNAVAILABLE;
    outcome.success = false;
    return outcome;
  }
  outcome.success = true;
  return outcome;
}

std::string collector_status_text(const char* name, const grpc::Status& status) {
  std::string text = std::string(name) + " collector: ";
  if (status.error_message().empty()) {
    text += "status " + std::to_string(static_cast<int>(status.error_code()));
    static constexpr std::pair<grpc::StatusCode, const char*> kNames[] = {
        {grpc::StatusCode::CANCELLED, "CANCELLED"}, {grpc::StatusCode::UNKNOWN, "UNKNOWN"},
        {grpc::StatusCode::INVALID_ARGUMENT, "INVALID_ARGUMENT"},
        {grpc::StatusCode::DEADLINE_EXCEEDED, "DEADLINE_EXCEEDED"},
        {grpc::StatusCode::NOT_FOUND, "NOT_FOUND"}, {grpc::StatusCode::ALREADY_EXISTS, "ALREADY_EXISTS"},
        {grpc::StatusCode::PERMISSION_DENIED, "PERMISSION_DENIED"},
        {grpc::StatusCode::RESOURCE_EXHAUSTED, "RESOURCE_EXHAUSTED"},
        {grpc::StatusCode::FAILED_PRECONDITION, "FAILED_PRECONDITION"},
        {grpc::StatusCode::ABORTED, "ABORTED"}, {grpc::StatusCode::OUT_OF_RANGE, "OUT_OF_RANGE"},
        {grpc::StatusCode::UNIMPLEMENTED, "UNIMPLEMENTED"}, {grpc::StatusCode::INTERNAL, "INTERNAL"},
        {grpc::StatusCode::UNAVAILABLE, "UNAVAILABLE"}, {grpc::StatusCode::DATA_LOSS, "DATA_LOSS"},
        {grpc::StatusCode::UNAUTHENTICATED, "UNAUTHENTICATED"}};
    for (const auto& [code, label] : kNames) {
      if (code == status.error_code()) text += std::string(" (") + label + ")";
    }
    return text;
  }
  return text + status.error_message();
}
}  // namespace grparse
