// Fuzzes the wiki storage door (COLLECTOR_CONFLUENCE): the hand-written
// XHTML pull parser over the request bytes and the fold of its tree into a
// Document, the same two steps parse_confluence_storage runs. The harness
// drives them without that entry point's catch-all, so an exception the
// fold throws (which the service would turn into an INTERNAL outcome) is a
// finding here instead of a silent failure.
//
// The contract under test: any body parses (damage is a warning, never an
// exception), and the Document the fold builds is whole: every reference
// resolves, every child names its parent (docling_integrity_errors), and it
// passes the docling-core structural rules an ENFORCE request checks
// (docling_structure_findings), all but one.
//
// The one left out is LIST_GROUP_NON_LIST_ITEM_CHILD, a known finding
// rather than a fuzz discovery: the fold hangs a nested list (and any other
// block written inside an <li>) off the enclosing list group, by design and
// under test (confluence_storage_test's verify_lists_nest_and_carry_markers),
// where docling-core wants it under the list item. Any nested list trips it,
// so checking it here would fail every run until the fold changes shape;
// fuzz/regressions/confluence-storage-fuzzer/ keeps the smallest input.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "confluence/storage_fold.h"
#include "confluence/storage_node.h"
#include "confluence/storage_parser.h"
#include "grparse/docling_map.h"
#include "grparse/structure_rules.h"

namespace docv1 = ai::pipestream::document::v1;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string bytes(reinterpret_cast<const char*>(data), size);
  std::vector<std::string> warnings;
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  document.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);

  grparse::confluence::StorageParser parser(bytes, &warnings);
  const grparse::confluence::Node root = parser.parse();
  if (!parser.saw_element()) return 0;  // not markup: the INVALID_ARGUMENT outcome
  grparse::confluence::StorageFold fold(&document, &warnings);
  fold.fold_blocks(root, "#/body");

  const std::vector<std::string> integrity = grparse::docling_integrity_errors(document);
  if (!integrity.empty()) {
    std::fprintf(stderr, "integrity: %s\n", integrity.front().c_str());
    __builtin_trap();
  }
  static const std::set<grparse::StructureRule> kChecked = [] {
    std::set<grparse::StructureRule> rules(std::begin(grparse::kAllStructureRules),
                                           std::end(grparse::kAllStructureRules));
    rules.erase(grparse::StructureRule::kListGroupNonListItemChild);
    return rules;
  }();
  const std::vector<grparse::StructureFinding> findings =
      grparse::docling_structure_findings(document, kChecked);
  if (!findings.empty()) {
    std::fprintf(stderr, "structure: %s\n", findings.front().message.c_str());
    __builtin_trap();
  }
  return 0;
}
