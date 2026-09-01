// What annotates a document rather than being part of it: comments and
// their threads, tracked changes, and bookmarks. Each one anchors in the
// document-absolute character space, so the fold records the anchor and
// leaves the resolution to the anchor index.
#pragma once

#include <string>

#include "grparse/office_fold/fold_base.h"
#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

class AnnotationFold : public FoldBase {
 public:
  AnnotationFold(DocumentArena& arena, AnchorIndex& anchors)
      : FoldBase(arena, anchors) {}

  void on_comment(const officev1::Comment& comment);
  void on_tracked_change(const officev1::TrackedChange& change);
  void on_bookmark(const officev1::Bookmark& bookmark);

 private:
  // The document-level comment section (Writer comments and slide
  // annotations), created when the first comment arrives.
  std::string comments_group_ref_;
};

}  // namespace grparse::office_fold
