#include "structure_validation.h"

#include <set>
#include <string>
#include <vector>

#include "grparse/structure_rules.h"

namespace grparse {

namespace parsev1 = ai::pipestream::parse::v1;

namespace {

// Findings named in an ENFORCE status before the rest are only counted.
constexpr size_t kListedFindings = 20;

std::string rule_wire_name(int raw) {
  std::string name = parsev1::StructureRule_Name(static_cast<parsev1::StructureRule>(raw));
  return name.empty() ? std::to_string(raw) : name;
}

}  // namespace

StructureRequest StructureRequest::from(const parsev1::ConvertDocumentOptions& options) {
  StructureRequest request;
  if (options.has_structure_validation()) request.validation = options.structure_validation();
  request.rules = options.structure_validation_rules();
  if (options.has_structure_repairs()) request.repairs = options.structure_repairs();
  return request;
}

bool StructureRequest::validates() const {
  return validation == parsev1::STRUCTURE_VALIDATION_REPORT ||
         validation == parsev1::STRUCTURE_VALIDATION_ENFORCE;
}

bool StructureRequest::repairs_anything() const {
  return repairs.has_value() &&
         (repairs->migrate_furniture_tree() || repairs->repair_referenced_orphans() ||
          repairs->wrap_list_children() || repairs->remove_empty_groups());
}

grpc::Status validate_structure_request(const StructureRequest& request,
                                        const std::string& surface) {
  if (!parsev1::StructureValidation_IsValid(request.validation)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + " structure_validation has invalid value '" +
                            std::to_string(request.validation) + "'");
  }
  for (const int raw : request.rules) {
    if (!parsev1::StructureRule_IsValid(raw) || raw == parsev1::STRUCTURE_RULE_UNSPECIFIED) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          surface + " structure_validation_rules contains invalid value '" +
                              rule_wire_name(raw) + "'");
    }
  }
  if (!request.rules.empty() && !request.validates()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        surface + ": structure_validation_rules needs structure_validation "
                                  "REPORT or ENFORCE");
  }
  return grpc::Status::OK;
}

std::optional<RepairOptions> repair_for_request(const std::optional<RepairOptions>& server,
                                                const StructureRequest& request) {
  if (!request.repairs_anything()) return server;
  RepairOptions options;
  if (server.has_value()) {
    options = *server;
  } else {
    // The server pass is off (GRPARSE_REPAIR=off): run only what the
    // request asked for.
    options.demote_running_furniture = false;
    options.rejoin_hyphenation = false;
    options.merge_continuations = false;
    options.infer_heading_hierarchy = false;
    options.order_body_by_geometry = false;
    options.split_paragraphs = false;
  }
  options.migrate_furniture_tree = request.repairs->migrate_furniture_tree();
  options.repair_referenced_orphans = request.repairs->repair_referenced_orphans();
  options.wrap_list_children = request.repairs->wrap_list_children();
  options.remove_empty_groups = request.repairs->remove_empty_groups();
  return options;
}

grpc::Status check_structure(const ai::pipestream::document::v1::Document& document,
                             const StructureRequest& request, const std::string& surface,
                             google::protobuf::RepeatedPtrField<parsev1::StructureFinding>* findings) {
  if (!request.validates()) return grpc::Status::OK;
  std::set<StructureRule> rules;
  for (const int raw : request.rules) rules.insert(static_cast<StructureRule>(raw));
  const std::vector<StructureFinding> found = docling_structure_findings(document, rules);
  if (request.validation == parsev1::STRUCTURE_VALIDATION_ENFORCE) {
    if (found.empty()) return grpc::Status::OK;
    std::string message = surface + ": the document breaks " + std::to_string(found.size()) +
                          " structural rule finding(s):";
    for (size_t index = 0; index < found.size() && index < kListedFindings; ++index) {
      message += "\n" + std::string(structure_rule_name(found[index].rule)) + ": " +
                 found[index].message;
    }
    if (found.size() > kListedFindings) {
      message += "\n... and " + std::to_string(found.size() - kListedFindings) + " more";
    }
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, message);
  }
  for (const StructureFinding& finding : found) {
    parsev1::StructureFinding* wire = findings->Add();
    wire->set_rule(static_cast<parsev1::StructureRule>(static_cast<int>(finding.rule)));
    wire->set_self_ref(finding.self_ref);
    if (!finding.related_ref.empty()) wire->set_related_ref(finding.related_ref);
    wire->set_message(finding.message);
  }
  return grpc::Status::OK;
}

}  // namespace grparse
