#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.pb.h"

namespace grparse {

// Rebuilds a Document from the streaming surface's page events, the way a
// client does (the fold DocumentRepairDelta's proto comment describes): each
// item at the arena index its self_ref names, page metadata into the page
// map, and the top-level body and furniture children recovered from the
// pages' body orders and parent refs. The server folds the pages it streams
// so it can tell clients what the whole-document passes changed.
class PageFold {
 public:
  void fold(const ai::pipestream::parse::v1::PageData& page);
  bool empty() const { return pages_ == 0; }
  const ai::pipestream::document::v1::Document& document() const { return document_; }

 private:
  ai::pipestream::document::v1::Document document_;
  std::unordered_set<std::string> body_;
  std::unordered_set<std::string> furniture_;
  int pages_ = 0;
};

// Sets each section header `levels` names (by self_ref) to its level: what
// a client does with the complete event's section_header_levels.
void apply_section_header_levels(const std::map<std::string, int32_t>& levels,
                                 ai::pipestream::document::v1::Document* document);

// What turns `before` (a fold of page events) into `after` (the same
// document once the whole-document passes ran), given the ref renumbering
// those passes applied (ReferenceRenameLog). Empty when `after` already is
// the fold, renamed.
std::optional<ai::pipestream::parse::v1::DocumentRepairDelta> repair_delta(
    const ai::pipestream::document::v1::Document& before,
    const ai::pipestream::document::v1::Document& after,
    const std::map<std::string, std::string>& renamed,
    ai::pipestream::parse::v1::Collector collector);

// Applies a delta to a fold, the client's side of repair_delta. False when
// the delta names an arena slot neither it nor the fold fills.
bool apply_repair_delta(const ai::pipestream::parse::v1::DocumentRepairDelta& delta,
                        ai::pipestream::document::v1::Document* document);

}  // namespace grparse
