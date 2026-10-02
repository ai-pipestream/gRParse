#pragma once

// The request side of the docling-core structural rules and repairs
// (docling-core #810): the wire options ConvertDocumentOptions and the
// streaming DocumentChunk carry, validated, turned into RepairOptions, and
// applied to a finished Document. Shared by the unary parse and the
// streaming collector events so both read the options the same way.
//
// Internal to src: the served surfaces stay in
// include/grparse/document_parser_service.h.

#include <optional>
#include <string>

#include <google/protobuf/repeated_field.h>
#include <grpcpp/grpcpp.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "grparse/document_repair.h"

namespace grparse {

// What one request asks of the structural rules: the mode, the rule
// subset, and the repairs. Built from either wire carrier.
struct StructureRequest {
  ai::pipestream::parse::v1::StructureValidation validation =
      ai::pipestream::parse::v1::STRUCTURE_VALIDATION_UNSPECIFIED;
  google::protobuf::RepeatedField<int> rules;
  std::optional<ai::pipestream::parse::v1::StructureRepairs> repairs;

  static StructureRequest from(const ai::pipestream::parse::v1::ConvertDocumentOptions& options);

  // True when REPORT or ENFORCE asks for the check.
  bool validates() const;
  // True when any repair switch is on.
  bool repairs_anything() const;
};

// INVALID_ARGUMENT naming the offender for a value outside either enum, an
// UNSPECIFIED rule, or a rule list given while validation is off (a
// filter on a check that does not run is a contradiction, not a no-op).
grpc::Status validate_structure_request(const StructureRequest& request,
                                        const std::string& surface);

// The repair pass one request runs: the server's pass (GRPARSE_REPAIR)
// with the request's structural switches added. When the server pass is
// off and the request turns a structural repair on, only the structural
// repairs run. nullopt when nothing runs.
std::optional<RepairOptions> repair_for_request(const std::optional<RepairOptions>& server,
                                                const StructureRequest& request);

// Checks `document` when the request validates. REPORT appends the typed
// findings to `findings` and returns OK; ENFORCE returns
// FAILED_PRECONDITION listing them (the first 20, then a count) when there
// is any. OFF does nothing.
grpc::Status check_structure(
    const ai::pipestream::document::v1::Document& document, const StructureRequest& request,
    const std::string& surface,
    google::protobuf::RepeatedPtrField<ai::pipestream::parse::v1::StructureFinding>* findings);

}  // namespace grparse
