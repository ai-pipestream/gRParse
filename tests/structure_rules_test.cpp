// Proves the docling-core structural rules (docling-core #810) as typed
// findings: each rule fires on a hand-built Document that breaks it and on
// nothing else, a well-formed document has none, a rule filter narrows the
// check, the orphan findings agree with the integrity check they are read
// from, and the request side (validation, REPORT, ENFORCE, the repair
// switches) behaves as the wire contract says.

#include <cctype>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../src/source_parse.h"
#include "../src/structure_validation.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "grparse/docling_map.h"
#include "grparse/structure_rules.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

namespace {

using grparse::StructureRule;
using grparse_test::require;
using grparse_test::require_equal;

docv1::Document empty_document() {
  docv1::Document document;
  document.set_name("rules.json");
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  return document;
}

// A text under `parent`, listed by it when `listed`.
std::string add_text(docv1::Document* document, const std::string& parent, bool listed = true,
                     docv1::DocItemLabel label = docv1::DOC_ITEM_LABEL_TEXT) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* base = document->add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref(parent);
  base->set_label(label);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text("text " + ref);
  if (listed) {
    if (parent == "#/body") {
      document->mutable_body()->add_children()->set_ref(ref);
    } else if (parent == "#/furniture") {
      document->mutable_furniture()->add_children()->set_ref(ref);
    } else if (parent.starts_with("#/groups/")) {
      document->mutable_groups(std::stoi(parent.substr(9)))->add_children()->set_ref(ref);
    }
  }
  return ref;
}

std::string add_list_item(docv1::Document* document, const std::string& parent) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* item = document->add_texts()->mutable_list_item();
  item->set_marker("-");
  auto* base = item->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref(parent);
  base->set_label(docv1::DOC_ITEM_LABEL_LIST_ITEM);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text("item " + ref);
  if (parent == "#/body") {
    document->mutable_body()->add_children()->set_ref(ref);
  } else if (parent.starts_with("#/groups/")) {
    document->mutable_groups(std::stoi(parent.substr(9)))->add_children()->set_ref(ref);
  }
  return ref;
}

std::string add_group(docv1::Document* document, docv1::GroupLabel label,
                      const std::string& parent = "#/body") {
  const std::string ref = "#/groups/" + std::to_string(document->groups_size());
  auto* group = document->add_groups();
  group->set_self_ref(ref);
  group->mutable_parent()->set_ref(parent);
  group->set_label(label);
  group->set_content_layer(docv1::CONTENT_LAYER_BODY);
  if (parent == "#/body") document->mutable_body()->add_children()->set_ref(ref);
  return ref;
}

std::vector<StructureRule> rules_of(const std::vector<grparse::StructureFinding>& findings) {
  std::vector<StructureRule> rules;
  for (const auto& finding : findings) rules.push_back(finding.rule);
  return rules;
}

// The one finding a document built to break one rule must produce, by
// value: the findings are usually a temporary.
grparse::StructureFinding only_finding(
    const std::vector<grparse::StructureFinding>& findings, StructureRule rule) {
  require_equal(findings.size(), size_t{1},
                "exactly one finding for " + std::string(grparse::structure_rule_name(rule)));
  require(findings[0].rule == rule,
          "the finding is " + std::string(grparse::structure_rule_name(findings[0].rule)) +
              ", not " + std::string(grparse::structure_rule_name(rule)));
  return findings[0];
}

void verify_well_formed_document_has_no_findings() {
  docv1::Document document = empty_document();
  add_text(&document, "#/body");
  const std::string list = add_group(&document, docv1::GROUP_LABEL_LIST);
  add_list_item(&document, list);
  add_list_item(&document, list);
  require(grparse::docling_integrity_errors(document).empty(), "the fixture is well linked");
  require(grparse::docling_structure_findings(document).empty(),
          "a well-formed document breaks no rule");
  require(grparse::docling_structure_findings(empty_document()).empty(),
          "an empty body is a valid empty document");
}

void verify_deprecated_furniture_tree() {
  docv1::Document document = empty_document();
  add_text(&document, "#/body");
  add_text(&document, "#/furniture", true, docv1::DOC_ITEM_LABEL_PAGE_HEADER);
  const auto& finding = only_finding(grparse::docling_structure_findings(document),
                                     StructureRule::kDeprecatedFurnitureTree);
  require_equal(finding.self_ref, std::string("#/furniture"), "the finding names the tree");
  require_equal(finding.message, std::string("Deprecated furniture node #/furniture has children"),
                "docling-core's wording");
}

void verify_key_value_and_form_items() {
  docv1::Document document = empty_document();
  auto* kv = document.add_key_value_items();
  kv->set_self_ref("#/key_value_items/0");
  kv->mutable_parent()->set_ref("#/body");
  document.mutable_body()->add_children()->set_ref("#/key_value_items/0");
  auto* form = document.add_form_items();
  form->set_self_ref("#/form_items/0");
  form->mutable_parent()->set_ref("#/body");
  document.mutable_body()->add_children()->set_ref("#/form_items/0");
  const auto findings = grparse::docling_structure_findings(document);
  require(rules_of(findings) == std::vector<StructureRule>{StructureRule::kKeyValueOrFormItem,
                                                           StructureRule::kKeyValueOrFormItem},
          "one finding per key-value and per form item");
  require_equal(findings[0].message,
                std::string("Key-value item #/key_value_items/0 is to be migrated to a field "
                            "region"),
                "the key-value wording");
  require_equal(findings[1].self_ref, std::string("#/form_items/0"), "the form item is named");
}

void verify_list_group_with_non_list_item_child() {
  docv1::Document document = empty_document();
  const std::string list = add_group(&document, docv1::GROUP_LABEL_ORDERED_LIST);
  add_list_item(&document, list);
  const std::string stray = add_text(&document, list);
  const auto& finding = only_finding(grparse::docling_structure_findings(document),
                                     StructureRule::kListGroupNonListItemChild);
  require_equal(finding.self_ref, list, "the finding names the list group");
  require_equal(finding.related_ref, stray, "and the offending child");
}

void verify_list_item_outside_list_group() {
  docv1::Document document = empty_document();
  const std::string loose = add_list_item(&document, "#/body");
  const auto& finding = only_finding(grparse::docling_structure_findings(document),
                                     StructureRule::kListItemOutsideListGroup);
  require_equal(finding.self_ref, loose, "the list item is named");
  require_equal(finding.related_ref, std::string("#/body"), "with the parent it has instead");

  // A plain text labelled LIST_ITEM is a list item for the rules too.
  docv1::Document labelled = empty_document();
  add_text(&labelled, "#/body", true, docv1::DOC_ITEM_LABEL_LIST_ITEM);
  only_finding(grparse::docling_structure_findings(labelled),
               StructureRule::kListItemOutsideListGroup);
}

void verify_empty_group() {
  docv1::Document document = empty_document();
  const std::string empty = add_group(&document, docv1::GROUP_LABEL_SECTION);
  const auto& finding =
      only_finding(grparse::docling_structure_findings(document), StructureRule::kEmptyGroup);
  require_equal(finding.self_ref, empty, "the empty group is named");
  require_equal(finding.message, "Group " + empty + " has no children", "docling-core's wording");
}

void verify_orphans() {
  docv1::Document document = empty_document();
  const std::string unlisted = add_text(&document, "#/body", false);
  const auto& not_listed = only_finding(grparse::docling_structure_findings(document),
                                        StructureRule::kNotListedByParent);
  require_equal(not_listed.self_ref, unlisted, "the orphan is named");
  require_equal(not_listed.related_ref, std::string("#/body"), "with the parent it claims");

  docv1::Document missing = empty_document();
  const std::string lost = add_text(&missing, "#/groups/7", false);
  const auto& parent_missing = only_finding(grparse::docling_structure_findings(missing),
                                            StructureRule::kParentMissing);
  require_equal(parent_missing.self_ref, lost, "the orphan is named");
  require_equal(parent_missing.message, lost + " has non-existent parent #/groups/7",
                "docling-core's wording");

  // The orphan rules are the integrity check's parent-link findings, typed:
  // both see the same two problems, the strings unchanged.
  const auto errors = grparse::docling_integrity_errors(missing);
  require(errors.size() == 1 && errors[0] == "parent #/groups/7 of " + lost + " does not resolve",
          "the integrity check's wording is unchanged");
}

// docling-core validate_tree: a node listed twice (a DAG) or a child that
// lists an ancestor (a cycle) is a repeated reference, named at the node
// that lists it again; the walk does not descend twice, so a cycle ends.
void verify_repeated_references() {
  const std::set<StructureRule> only{StructureRule::kRepeatedReference};
  {
    docv1::Document document = empty_document();
    const std::string section = add_group(&document, docv1::GROUP_LABEL_SECTION);
    const std::string text = add_text(&document, section);
    document.mutable_body()->add_children()->set_ref(text);
    const auto finding =
        only_finding(grparse::docling_structure_findings(document, only),
                     StructureRule::kRepeatedReference);
    require(finding.self_ref == "#/body" && finding.related_ref == text &&
                finding.message.contains("already reachable through another reference"),
            "a node listed by two parents is found where the walk meets it again: " +
                finding.message);
  }
  {
    docv1::Document document = empty_document();
    const std::string outer = add_group(&document, docv1::GROUP_LABEL_SECTION);
    const std::string inner = add_group(&document, docv1::GROUP_LABEL_SECTION, outer);
    document.mutable_groups(1)->mutable_parent()->set_ref(outer);
    document.mutable_groups(0)->add_children()->set_ref(inner);
    document.mutable_groups(1)->add_children()->set_ref(outer);
    const auto finding =
        only_finding(grparse::docling_structure_findings(document, only),
                     StructureRule::kRepeatedReference);
    require(finding.self_ref == inner && finding.related_ref == outer,
            "a cycle is found at the back edge and the walk ends");
  }
  {
    docv1::Document document = empty_document();
    const std::string text = add_text(&document, "#/body");
    document.mutable_body()->add_children()->set_ref("#/body");
    document.mutable_body()->add_children()->set_ref(text);
    const auto findings = grparse::docling_structure_findings(document, only);
    require(findings.size() == 2 && findings[0].related_ref == "#/body" &&
                findings[1].related_ref == text,
            "the root is visited from the start, and each repeat is its own finding");
  }
}

// A rich cell is owned by its table: one item per cell, parented by the
// table, and listed among its children (docling-core validate_tree).
void verify_rich_cells_are_owned_by_their_table() {
  const std::set<StructureRule> only{StructureRule::kRichCellNotOwned};
  const auto table_with = [](docv1::Document* document) {
    auto* table = document->add_tables();
    table->set_self_ref("#/tables/0");
    table->mutable_parent()->set_ref("#/body");
    table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
    table->mutable_data()->set_num_rows(1);
    table->mutable_data()->set_num_cols(2);
    document->mutable_body()->add_children()->set_ref("#/tables/0");
    return table;
  };
  {
    docv1::Document document = empty_document();
    auto* table = table_with(&document);
    const std::string cell = add_text(&document, "#/tables/0", false);
    table->add_children()->set_ref(cell);
    table->mutable_data()->add_table_cells()->mutable_ref()->set_ref(cell);
    require(grparse::docling_structure_findings(document, only).empty(),
            "an owned rich cell breaks nothing");
    document.mutable_tables(0)->mutable_data()->add_table_cells()->mutable_ref()->set_ref(cell);
    const auto finding = only_finding(grparse::docling_structure_findings(document, only),
                                      StructureRule::kRichCellNotOwned);
    require(finding.self_ref == "#/tables/0" && finding.related_ref == cell &&
                finding.message.contains("shared"),
            "two cells naming one item are a shared rich cell");
  }
  {
    docv1::Document document = empty_document();
    auto* table = table_with(&document);
    const std::string cell = add_text(&document, "#/body");
    table->mutable_data()->add_table_cells()->mutable_ref()->set_ref(cell);
    const auto finding = only_finding(grparse::docling_structure_findings(document, only),
                                      StructureRule::kRichCellNotOwned);
    require(finding.message.contains("with parent #/body"),
            "a cell item parented elsewhere is not owned: " + finding.message);
  }
  {
    docv1::Document document = empty_document();
    auto* table = table_with(&document);
    const std::string cell = add_text(&document, "#/tables/0", false);
    table->mutable_data()->add_table_cells()->mutable_ref()->set_ref(cell);
    const auto finding = only_finding(grparse::docling_structure_findings(document, only),
                                      StructureRule::kRichCellNotOwned);
    require(finding.message.contains("does not list"),
            "a cell item the table does not list is not owned: " + finding.message);
  }
}

// docling-core's grid limits: per table on rows, columns and their product,
// and per document on the sum; defaults 1,000,000 and 5,000,000.
void verify_table_cell_limits() {
  const auto add_table = [](docv1::Document* document, int rows, int cols) {
    const std::string ref = "#/tables/" + std::to_string(document->tables_size());
    auto* table = document->add_tables();
    table->set_self_ref(ref);
    table->mutable_parent()->set_ref("#/body");
    table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
    table->mutable_data()->set_num_rows(rows);
    table->mutable_data()->set_num_cols(cols);
    document->mutable_body()->add_children()->set_ref(ref);
    return ref;
  };
  const std::set<StructureRule> per_table{StructureRule::kTableCellsOverLimit};
  const std::set<StructureRule> total{StructureRule::kTotalTableCellsOverLimit};
  {
    docv1::Document document = empty_document();
    add_table(&document, 1000, 1000);
    add_table(&document, 1'000'000, 0);
    require(grparse::docling_structure_findings(document, per_table).empty(),
            "exactly the limit, and a limit-sized dimension with an empty other, pass");
    const std::string over = add_table(&document, 1000, 1001);
    const auto finding = only_finding(grparse::docling_structure_findings(document, per_table),
                                      StructureRule::kTableCellsOverLimit);
    require(finding.self_ref == over &&
                finding.message == "Table of 1000x1001 cells exceeds the limit of 1000000 cells",
            "docling-core's wording: " + finding.message);
    add_table(&document, 1'000'001, 0);
    add_table(&document, -5, 10);
    require_equal(grparse::docling_structure_findings(document, per_table).size(), size_t{2},
                  "a dimension alone over the limit counts, a negative one is zero");
  }
  {
    docv1::Document document = empty_document();
    for (int index = 0; index < 5; ++index) add_table(&document, 1000, 1000);
    require(grparse::docling_structure_findings(document, total).empty(),
            "five tables at the per-table limit are exactly the total limit");
    add_table(&document, 1, 1);
    const auto finding = only_finding(grparse::docling_structure_findings(document, total),
                                      StructureRule::kTotalTableCellsOverLimit);
    require(finding.self_ref == "#/tables" &&
                finding.message ==
                    "Document tables declare 5000001 cells in total, exceeding the limit of "
                    "5000000",
            "docling-core's wording: " + finding.message);
  }
  {
    docv1::Document document = empty_document();
    add_table(&document, 3, 4);
    add_table(&document, 2, 2);
    grparse::StructureCellLimits limits;
    limits.max_table_cells = 10;
    limits.max_total_table_cells = 15;
    const std::set<StructureRule> both{StructureRule::kTableCellsOverLimit,
                                       StructureRule::kTotalTableCellsOverLimit};
    require(rules_of(grparse::docling_structure_findings(document, both, limits)) ==
                std::vector<StructureRule>{StructureRule::kTableCellsOverLimit,
                                           StructureRule::kTotalTableCellsOverLimit},
            "the request's limits replace the defaults");
  }
  {
    const std::string surface = "ConvertSource";
    parsev1::ConvertDocumentOptions options;
    options.set_structure_validation(parsev1::STRUCTURE_VALIDATION_REPORT);
    options.mutable_structure_table_cell_limits()->set_max_table_cells(10);
    require(grparse::validate_options(options, surface).ok(), "positive limits are accepted");
    options.mutable_structure_table_cell_limits()->set_max_total_table_cells(0);
    const grpc::Status zero = grparse::validate_options(options, surface);
    require(zero.error_code() == grpc::StatusCode::INVALID_ARGUMENT &&
                zero.error_message().contains("max_total_table_cells must be at least 1"),
            "a zero limit is OUT_OF_RANGE: " + zero.error_message());
    parsev1::ConvertDocumentOptions off;
    off.mutable_structure_table_cell_limits()->set_max_table_cells(10);
    const grpc::Status contradiction = grparse::validate_options(off, surface);
    require(contradiction.error_code() == grpc::StatusCode::INVALID_ARGUMENT &&
                contradiction.error_message().contains("needs structure_validation"),
            "limits without validation are contradictory: " + contradiction.error_message());

    docv1::Document document = empty_document();
    add_table(&document, 4, 4);
    google::protobuf::RepeatedPtrField<parsev1::StructureFinding> findings;
    parsev1::ConvertDocumentOptions report;
    report.set_structure_validation(parsev1::STRUCTURE_VALIDATION_REPORT);
    report.mutable_structure_table_cell_limits()->set_max_table_cells(10);
    require(grparse::check_structure(document, grparse::StructureRequest::from(report), surface,
                                     &findings)
                    .ok() &&
                findings.size() == 1 &&
                findings[0].rule() == parsev1::STRUCTURE_RULE_TABLE_CELLS_OVER_LIMIT,
            "REPORT reads the request's limits");
  }
}

void verify_rule_filter_and_order() {
  docv1::Document document = empty_document();
  add_text(&document, "#/furniture", true, docv1::DOC_ITEM_LABEL_PAGE_FOOTER);
  add_group(&document, docv1::GROUP_LABEL_SECTION);
  add_text(&document, "#/body", false);
  const auto all = grparse::docling_structure_findings(document);
  require(rules_of(all) == std::vector<StructureRule>{StructureRule::kDeprecatedFurnitureTree,
                                                      StructureRule::kEmptyGroup,
                                                      StructureRule::kNotListedByParent},
          "the furniture tree first, then node by node in arena order");
  const auto filtered = grparse::docling_structure_findings(
      document, {StructureRule::kEmptyGroup, StructureRule::kParentMissing});
  require(rules_of(filtered) == std::vector<StructureRule>{StructureRule::kEmptyGroup},
          "the filter keeps only the rules it names");
}

// The C++ ids and the wire enum agree value for value, so the service maps
// one onto the other by number.
void verify_rule_ids_match_the_wire() {
  for (const StructureRule rule : grparse::kAllStructureRules) {
    const int value = static_cast<int>(rule);
    require(parsev1::StructureRule_IsValid(value), "every rule id is a wire value");
    std::string wire = parsev1::StructureRule_Name(static_cast<parsev1::StructureRule>(value));
    std::string expected = "STRUCTURE_RULE_" + std::string(grparse::structure_rule_name(rule));
    for (char& c : expected) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    require_equal(wire, expected, "the wire name of rule " + std::to_string(value));
  }
  require_equal(static_cast<size_t>(parsev1::StructureRule_descriptor()->value_count()),
                std::size(grparse::kAllStructureRules) + 1, "every wire rule has a C++ id");
}

void verify_request_validation() {
  const std::string surface = "ConvertSource";
  parsev1::ConvertDocumentOptions options;
  options.set_structure_validation(parsev1::STRUCTURE_VALIDATION_REPORT);
  options.add_structure_validation_rules(parsev1::STRUCTURE_RULE_EMPTY_GROUP);
  options.mutable_structure_repairs()->set_remove_empty_groups(true);
  require(grparse::validate_options(options, surface).ok(),
          "the three structural options are implemented");

  parsev1::ConvertDocumentOptions contradictory;
  contradictory.add_structure_validation_rules(parsev1::STRUCTURE_RULE_EMPTY_GROUP);
  const grpc::Status off = grparse::validate_options(contradictory, surface);
  require(off.error_code() == grpc::StatusCode::INVALID_ARGUMENT &&
              off.error_message().contains("needs structure_validation"),
          "a rule filter without validation is rejected: " + off.error_message());

  parsev1::ConvertDocumentOptions unspecified;
  unspecified.set_structure_validation(parsev1::STRUCTURE_VALIDATION_ENFORCE);
  unspecified.add_structure_validation_rules(parsev1::STRUCTURE_RULE_UNSPECIFIED);
  const grpc::Status bad_rule = grparse::validate_options(unspecified, surface);
  require(bad_rule.error_code() == grpc::StatusCode::INVALID_ARGUMENT &&
              bad_rule.error_message().contains("STRUCTURE_RULE_UNSPECIFIED"),
          "an unspecified rule is rejected by name: " + bad_rule.error_message());

  parsev1::ConvertDocumentOptions unknown;
  unknown.set_structure_validation(static_cast<parsev1::StructureValidation>(42));
  const grpc::Status bad_mode = grparse::validate_options(unknown, surface);
  require(bad_mode.error_code() == grpc::StatusCode::INVALID_ARGUMENT &&
              bad_mode.error_message().contains("42"),
          "an unknown mode is rejected: " + bad_mode.error_message());
}

void verify_report_and_enforce() {
  docv1::Document document = empty_document();
  const std::string empty = add_group(&document, docv1::GROUP_LABEL_SECTION);
  add_text(&document, "#/body", false);
  google::protobuf::RepeatedPtrField<parsev1::StructureFinding> findings;

  parsev1::ConvertDocumentOptions off;
  require(grparse::check_structure(document, grparse::StructureRequest::from(off), "ConvertSource",
                                   &findings)
                  .ok() &&
              findings.empty(),
          "unset validation checks nothing");

  parsev1::ConvertDocumentOptions report;
  report.set_structure_validation(parsev1::STRUCTURE_VALIDATION_REPORT);
  require(grparse::check_structure(document, grparse::StructureRequest::from(report),
                                   "ConvertSource", &findings)
              .ok(),
          "REPORT succeeds");
  require_equal(findings.size(), 2, "REPORT returns both findings");
  require(findings[0].rule() == parsev1::STRUCTURE_RULE_EMPTY_GROUP &&
              findings[0].self_ref() == empty && !findings[0].has_related_ref(),
          "the empty group finding is typed");
  require(findings[1].rule() == parsev1::STRUCTURE_RULE_NOT_LISTED_BY_PARENT &&
              findings[1].related_ref() == "#/body",
          "the orphan finding carries its parent");

  findings.Clear();
  report.add_structure_validation_rules(parsev1::STRUCTURE_RULE_PARENT_MISSING);
  require(grparse::check_structure(document, grparse::StructureRequest::from(report),
                                   "ConvertSource", &findings)
                  .ok() &&
              findings.empty(),
          "a filter that names no broken rule reports nothing");

  parsev1::ConvertDocumentOptions enforce;
  enforce.set_structure_validation(parsev1::STRUCTURE_VALIDATION_ENFORCE);
  const grpc::Status failed = grparse::check_structure(
      document, grparse::StructureRequest::from(enforce), "ConvertSource", &findings);
  require(failed.error_code() == grpc::StatusCode::FAILED_PRECONDITION,
          "ENFORCE fails the request");
  require(failed.error_message().contains("empty_group: Group " + empty + " has no children") &&
              failed.error_message().contains("not_listed_by_parent"),
          "ENFORCE lists the findings: " + failed.error_message());
  require(findings.empty(), "ENFORCE returns no findings beside the status");

  docv1::Document clean = empty_document();
  add_text(&clean, "#/body");
  require(grparse::check_structure(clean, grparse::StructureRequest::from(enforce),
                                   "ConvertSource", &findings)
              .ok(),
          "ENFORCE passes a document that breaks nothing");
}

void verify_repair_switches() {
  const std::optional<grparse::RepairOptions> server = grparse::RepairOptions{};
  parsev1::ConvertDocumentOptions none;
  const auto untouched = grparse::repair_for_request(server, grparse::StructureRequest::from(none));
  require(untouched.has_value() && !untouched->migrate_furniture_tree &&
              !untouched->remove_empty_groups,
          "no request switch keeps the server pass as configured");
  require(!grparse::repair_for_request(std::nullopt, grparse::StructureRequest::from(none))
               .has_value(),
          "a server with the pass off and no switch runs nothing");

  parsev1::ConvertDocumentOptions asked;
  asked.mutable_structure_repairs()->set_migrate_furniture_tree(true);
  asked.mutable_structure_repairs()->set_wrap_list_children(true);
  const auto on = grparse::repair_for_request(server, grparse::StructureRequest::from(asked));
  require(on.has_value() && on->migrate_furniture_tree && on->wrap_list_children &&
              !on->repair_referenced_orphans && !on->remove_empty_groups &&
              on->demote_running_furniture,
          "the request's switches join the server pass");
  const auto only = grparse::repair_for_request(std::nullopt, grparse::StructureRequest::from(asked));
  require(only.has_value() && only->migrate_furniture_tree && !only->demote_running_furniture &&
              !only->merge_continuations && !only->rejoin_hyphenation &&
              !only->infer_heading_hierarchy && !only->order_body_by_geometry &&
              !only->split_paragraphs,
          "with the server pass off, only the requested structural repairs run");

  parsev1::ConvertDocumentOptions all_false;
  all_false.mutable_structure_repairs()->set_remove_empty_groups(false);
  require(!grparse::StructureRequest::from(all_false).repairs_anything(),
          "an all-false repairs message asks for nothing");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("structure-rules-test", "ok", {
      verify_well_formed_document_has_no_findings,
      verify_deprecated_furniture_tree,
      verify_key_value_and_form_items,
      verify_list_group_with_non_list_item_child,
      verify_list_item_outside_list_group,
      verify_empty_group,
      verify_orphans,
      verify_repeated_references,
      verify_rich_cells_are_owned_by_their_table,
      verify_table_cell_limits,
      verify_rule_filter_and_order,
      verify_rule_ids_match_the_wire,
      verify_request_validation,
      verify_report_and_enforce,
      verify_repair_switches,
  });
}
