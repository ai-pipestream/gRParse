// What every part fold is built from: the arena it appends to, the anchor
// index it registers spans with, and the one text-filling step they share.
#pragma once

#include <string>

#include "grparse/office_fold/anchor_index.h"
#include "grparse/office_fold/arena.h"
#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

class FoldBase {
 protected:
  FoldBase(DocumentArena& arena, AnchorIndex& anchors)
      : arena_(arena), anchors_(anchors) {}

  // The text of one run sequence on a freshly added item: its text and
  // orig, the item-level formatting a uniform sequence carries, its inline
  // spans, and the item-level hyperlink. Returns the text, for callers that
  // measure it.
  std::string fill_from_runs(const TextRuns& runs, const TextHandle& handle);

  // The inline spans of one run sequence, for items assembled from several
  // of them.
  void add_spans(const TextRuns& runs, const TextHandle& handle,
                 long long base_offset);

  DocumentArena& arena_;
  AnchorIndex& anchors_;
};

}  // namespace grparse::office_fold
