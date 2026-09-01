#include "grparse/office_fold/annotation_fold.h"

#include <algorithm>
#include <cctype>

#include "grparse/office_fold/run_text.h"
#include "grparse/office_fold/value_convert.h"

namespace grparse::office_fold {

namespace {

// Who wrote the comment, when, and what it says about itself.
void set_comment_identity(const officev1::Comment& comment,
                          docv1::CommentMeta* identity) {
  if (!comment.author().empty()) identity->set_author(comment.author());
  if (!comment.initials().empty()) identity->set_initials(comment.initials());
  if (comment.epoch_ms() != 0) {
    set_instant(comment.epoch_ms(), identity->mutable_timestamp());
  }
  identity->set_resolved(comment.resolved());
  if (!comment.anchored_text().empty()) {
    identity->set_anchored_text(comment.anchored_text());
  }
}

}  // namespace

void AnnotationFold::on_comment(const officev1::Comment& comment) {
  if (comments_group_ref_.empty()) {
    comments_group_ref_ =
        arena_.add_group("#/furniture", docv1::GROUP_LABEL_COMMENT_SECTION,
                         "comments", docv1::CONTENT_LAYER_FURNITURE)
            ->self_ref();
  }
  TextHandle handle = arena_.add_text(TextKind::kText,
                                      docv1::DOC_ITEM_LABEL_TEXT,
                                      docv1::CONTENT_LAYER_FURNITURE,
                                      comments_group_ref_);
  const std::string text =
      !comment.text().empty() ? comment.text() : concat_runs(comment.runs());
  handle.base->set_text(text);
  handle.base->set_orig(text);
  set_comment_identity(comment, handle.base->mutable_comment_meta());
  // The office core's comment name is the threading key and nothing else,
  // so it becomes a reference to the parent comment rather than a string a
  // consumer would have to match itself.
  if (!comment.name().empty()) {
    anchors_.name_comment(comment.name(), handle.ref);
  }
  if (!comment.parent_name().empty()) {
    anchors_.add_comment_reply(handle.ref, comment.parent_name());
  }
  if (comment.char_start() >= 0) {
    // The item this comment annotates is not known yet: a comment can close
    // before the paragraph holding it is emitted. The back-link is made
    // once the whole body has streamed past.
    anchors_.add_comment(handle.ref, comment.char_start(),
                         comment.char_end());
  }
  arena_.add_caret_prov(handle.base->mutable_prov(), comment.page_index(),
                        comment.anchor(), comment.anchor(), 0,
                        runs_length(comment.runs()));
}

void AnnotationFold::on_tracked_change(const officev1::TrackedChange& change) {
  // Tracked changes annotate spans of the body text rather than adding
  // display text of their own, so they are records of the document, each
  // pointing at the item and range it touches.
  const int index = arena_.document().changes_size();
  docv1::ChangeRecord* record = arena_.document().add_changes();
  record->set_id(change.identifier().empty()
                     ? std::to_string(change.index())
                     : change.identifier());
  std::string kind = change.kind_name();
  std::ranges::transform(kind, kind.begin(),
                         [](unsigned char c) { return std::tolower(c); });
  record->set_kind(kind);
  record->set_author(change.author());
  if (change.epoch_ms() != 0) {
    set_instant(change.epoch_ms(), record->mutable_timestamp());
  }
  if (!change.changed_text().empty()) {
    record->set_content(change.changed_text());
  }
  anchors_.add_change(index, change.char_start(), change.char_end());
}

void AnnotationFold::on_bookmark(const officev1::Bookmark& bookmark) {
  // A bookmark names a position other content points at, so it becomes a
  // named anchor; the target resolves once the body index is complete.
  anchors_.add_named_anchor(bookmark.name(), bookmark.char_start(),
                            bookmark.char_end());
}

}  // namespace grparse::office_fold
