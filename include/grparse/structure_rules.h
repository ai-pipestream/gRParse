// The docling-core structural rules (DoclingDocument._validate_rules, as of
// docling-core #810) over an ai.pipestream.document.v1.Document, as typed
// findings rather than raised ValueErrors.
//
// The integrity check (docling_integrity_errors in docling_map.h) is the
// fold's own contract: references resolve, parents list their children,
// provenance names a page. These rules sit on top of it. The two orphan
// rules are the integrity check's parent-link findings, read from the same
// walk and typed; the other rules are shape constraints docling-core adds
// over a well-linked tree (no deprecated furniture tree, list groups hold
// list items, no empty groups, no key-value or form items). A document can
// pass the integrity check and still break a rule: gRParse itself writes
// the furniture tree and form items today, so the rules are opt-in.
#ifndef GRPARSE_STRUCTURE_RULES_H
#define GRPARSE_STRUCTURE_RULES_H

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

// One id per docling-core check. The values match the wire enum
// ai.pipestream.parse.v1.StructureRule so the service maps them by value.
enum class StructureRule {
  kDeprecatedFurnitureTree = 1,
  kKeyValueOrFormItem = 2,
  kListGroupNonListItemChild = 3,
  kListItemOutsideListGroup = 4,
  kEmptyGroup = 5,
  kParentMissing = 6,
  kNotListedByParent = 7,
};

inline constexpr StructureRule kAllStructureRules[] = {
    StructureRule::kDeprecatedFurnitureTree,   StructureRule::kKeyValueOrFormItem,
    StructureRule::kListGroupNonListItemChild, StructureRule::kListItemOutsideListGroup,
    StructureRule::kEmptyGroup,                StructureRule::kParentMissing,
    StructureRule::kNotListedByParent,
};

struct StructureFinding {
  StructureRule rule{};
  // The item the finding is about.
  std::string self_ref;
  // The other end, when the finding has one: an orphan's parent, a list
  // item's parent, a list group's offending child.
  std::string related_ref;
  // docling-core's wording for the same finding.
  std::string message;
};

// A short stable name for logs and status messages ("empty_group").
std::string_view structure_rule_name(StructureRule rule);

// Every rule `document` breaks, restricted to `rules` when it is non-empty.
// Unlike docling-core, which checks the list rules only on items reachable
// from the body, every arena item is checked: an orphaned list item is
// still a list item. The order is fixed: the furniture tree, then key-value
// and form items in arena order, then each node's list, parent and empty
// group findings in arena order (body, furniture, groups, texts, pictures,
// tables, key-value items, form items, field regions, field items). Empty
// means the document follows every checked rule.
std::vector<StructureFinding> docling_structure_findings(
    const ai::pipestream::document::v1::Document& document,
    const std::set<StructureRule>& rules = {});

}  // namespace grparse

#endif
