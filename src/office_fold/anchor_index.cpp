#include "grparse/office_fold/anchor_index.h"

#include <algorithm>
#include <iterator>

#include "grparse/office_fold/value_convert.h"

namespace grparse::office_fold {

void AnchorIndex::add_body_span(long long start, long long end,
                                std::string ref) {
  body_spans_.push_back({start, end, std::move(ref)});
}

void AnchorIndex::add_comment(std::string ref, long long start,
                              long long end) {
  pending_comments_.push_back({std::move(ref), start, end});
}

void AnchorIndex::name_comment(const std::string& name,
                               const std::string& ref) {
  comment_ref_by_name_[name] = ref;
}

void AnchorIndex::add_comment_reply(std::string reply_ref,
                                    std::string parent_name) {
  pending_comment_parents_.emplace_back(std::move(reply_ref),
                                        std::move(parent_name));
}

void AnchorIndex::add_reference(std::string item_ref, int span_index,
                                std::string target_name) {
  pending_references_.push_back(
      {std::move(item_ref), span_index, std::move(target_name)});
}

void AnchorIndex::add_named_anchor(std::string name, long long start,
                                   long long end) {
  pending_anchors_.push_back({std::move(name), start, end});
}

void AnchorIndex::add_change(int index, long long start, long long end) {
  pending_changes_.push_back({index, start, end});
}

void AnchorIndex::add_field_span(int index, long long start, long long end) {
  pending_field_spans_.push_back({index, start, end});
}

bool AnchorIndex::resolve_doc_span(long long start, long long end,
                                   docv1::FineRef* out) const {
  if (start < 0) return false;
  // The index is built in arrival order, which is ascending offset order,
  // so the item holding an offset is the last one starting at or before it.
  auto after = std::upper_bound(
      body_spans_.begin(), body_spans_.end(), start,
      [](long long value, const BodySpan& span) { return value < span.start; });
  if (after == body_spans_.begin()) return false;
  const BodySpan& span = *std::prev(after);
  if (start >= span.end && start != span.start) return false;
  out->set_ref(span.ref);
  long long local_start = start - span.start;
  long long local_end = (end > start ? end : start) - span.start;
  long long length = span.end - span.start;
  out->mutable_range()->set_start(clamp32(std::min(local_start, length)));
  out->mutable_range()->set_end(clamp32(std::min(local_end, length)));
  return true;
}

void AnchorIndex::link_replies(DocumentArena& arena) const {
  // A reply points at the comment it answers, once that comment exists.
  for (const auto& [reply_ref, parent_name] : pending_comment_parents_) {
    auto parent = comment_ref_by_name_.find(parent_name);
    if (parent == comment_ref_by_name_.end()) continue;
    docv1::TextItemBase* base = arena.text_by_ref(reply_ref);
    if (base == nullptr) continue;
    base->mutable_comment_meta()->mutable_parent()->set_ref(parent->second);
  }
}

void AnchorIndex::back_link_comments(DocumentArena& arena) const {
  // The item a comment annotates gains a back-link carrying the annotated
  // range in that item's own characters.
  for (const PendingComment& pending : pending_comments_) {
    docv1::FineRef anchor;
    if (!resolve_doc_span(pending.start, pending.end, &anchor)) continue;
    docv1::TextItemBase* base = arena.text_by_ref(anchor.ref());
    if (base == nullptr) continue;
    docv1::FineRef* link = base->add_comments();
    link->set_ref(pending.ref);
    *link->mutable_range() = anchor.range();
  }
}

void AnchorIndex::place_field_spans(DocumentArena& arena) const {
  for (const PendingSpan& pending : pending_field_spans_) {
    if (pending.index >= arena.document().field_items_size()) continue;
    docv1::FineRef span;
    if (!resolve_doc_span(pending.start, pending.end, &span)) continue;
    *arena.document().mutable_field_items(pending.index)->mutable_span() = span;
  }
}

void AnchorIndex::target_changes(DocumentArena& arena) const {
  for (const PendingSpan& pending : pending_changes_) {
    if (pending.index >= arena.document().changes_size()) continue;
    docv1::ChangeRecord* change =
        arena.document().mutable_changes(pending.index);
    docv1::FineRef target;
    // An anchor in content the fold does not emit keeps its record and
    // loses only the target: an unanchored change is still evidence.
    if (resolve_doc_span(pending.start, pending.end, &target)) {
      *change->mutable_target() = target;
    }
  }
}

std::map<std::string, docv1::FineRef> AnchorIndex::place_named_anchors(
    DocumentArena& arena) const {
  std::map<std::string, docv1::FineRef> resolved;
  for (const PendingAnchor& pending : pending_anchors_) {
    docv1::NamedAnchor* anchor = arena.document().add_anchors();
    anchor->set_name(pending.name);
    docv1::FineRef target;
    if (resolve_doc_span(pending.start, pending.end, &target)) {
      *anchor->mutable_target() = target;
      resolved[pending.name] = target;
    }
  }
  return resolved;
}

void AnchorIndex::target_references(
    DocumentArena& arena,
    const std::map<std::string, docv1::FineRef>& anchors) const {
  // A cross-reference field points at a named anchor; now that every anchor
  // is placed, the field's span can point at the same item.
  for (const PendingReference& pending : pending_references_) {
    auto found = anchors.find(pending.target_name);
    if (found == anchors.end()) continue;
    docv1::TextItemBase* base = arena.text_by_ref(pending.item_ref);
    if (base == nullptr || pending.span_index >= base->spans_size()) continue;
    *base->mutable_spans(pending.span_index)->mutable_target() = found->second;
  }
}

void AnchorIndex::resolve(DocumentArena& arena) const {
  link_replies(arena);
  back_link_comments(arena);
  place_field_spans(arena);
  target_changes(arena);
  target_references(arena, place_named_anchors(arena));
}

}  // namespace grparse::office_fold
