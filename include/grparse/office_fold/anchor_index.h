// The document-absolute character space and everything that anchors in it.
//
// The office wire counts comments, tracked changes, bookmarks, and field
// marks in one document-absolute character space. This index grows while
// body items stream past and resolves every anchor against it once the
// stream ends: a comment back-links to the item it annotates, a tracked
// change and a bookmark each carry a FineRef into an item's own character
// range, and a cross-reference field points at the anchor it names. An
// anchor that falls in content the fold does not emit is kept without a
// target rather than dropped.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "grparse/office_fold/arena.h"
#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

class AnchorIndex {
 public:
  // One body item's extent in the character space, in arrival order (which
  // is ascending offset order).
  void add_body_span(long long start, long long end, std::string ref);

  // A comment item waiting for the body index to be complete so its
  // anchored item can be found and back-linked.
  void add_comment(std::string ref, long long start, long long end);
  // The office core's comment name is the threading key: a reply names the
  // comment it answers, which may not have arrived yet.
  void name_comment(const std::string& name, const std::string& ref);
  void add_comment_reply(std::string reply_ref, std::string parent_name);

  // A cross-reference span waiting for the anchor it names to be resolved.
  void add_reference(std::string item_ref, int span_index,
                     std::string target_name);
  // A named anchor and the range it covers.
  void add_named_anchor(std::string name, long long start, long long end);
  // A tracked change and the range it touches; the index is the change's
  // own arena position.
  void add_change(int index, long long start, long long end);
  // A form field and the range it covers; the index is the field's own
  // arena position.
  void add_field_span(int index, long long start, long long end);

  // Resolves a range of the character space to the item that holds it, with
  // the range rebased to that item's own text. False when no emitted item
  // covers the start of the range.
  bool resolve_doc_span(long long start, long long end,
                        docv1::FineRef* out) const;

  // Resolves everything that anchors in the character space, once the whole
  // body has streamed past.
  void resolve(DocumentArena& arena) const;

 private:
  struct BodySpan {
    long long start = 0;
    long long end = 0;
    std::string ref;
  };
  struct PendingComment {
    std::string ref;
    long long start = 0;
    long long end = 0;
  };
  struct PendingReference {
    std::string item_ref;
    int span_index = 0;
    std::string target_name;
  };
  struct PendingAnchor {
    std::string name;
    long long start = 0;
    long long end = 0;
  };
  // A pending span of the character space against an arena index: a form
  // field's own extent, or a tracked change's target.
  struct PendingSpan {
    int index = 0;
    long long start = 0;
    long long end = 0;
  };

  void link_replies(DocumentArena& arena) const;
  void back_link_comments(DocumentArena& arena) const;
  void place_field_spans(DocumentArena& arena) const;
  void target_changes(DocumentArena& arena) const;
  // Appends every named anchor, resolved where the body covers it, and
  // returns the ones a cross-reference can point at.
  std::map<std::string, docv1::FineRef> place_named_anchors(
      DocumentArena& arena) const;
  void target_references(
      DocumentArena& arena,
      const std::map<std::string, docv1::FineRef>& anchors) const;

  std::vector<BodySpan> body_spans_;
  std::vector<PendingComment> pending_comments_;
  // Comment items by the office core's comment name, and the reply links
  // waiting for the comment they name to arrive.
  std::map<std::string, std::string> comment_ref_by_name_;
  std::vector<std::pair<std::string, std::string>> pending_comment_parents_;
  std::vector<PendingReference> pending_references_;
  std::vector<PendingAnchor> pending_anchors_;
  std::vector<PendingSpan> pending_changes_;
  std::vector<PendingSpan> pending_field_spans_;
};

}  // namespace grparse::office_fold
