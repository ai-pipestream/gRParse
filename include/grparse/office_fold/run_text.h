// Text runs to document text: the concatenation, the item-level formatting
// a uniform run sequence carries, and the inline spans a mixed one needs.
#pragma once

#include <string>

#include "grparse/office_fold/anchor_index.h"
#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

std::string concat_runs(const TextRuns& runs);
long long runs_length(const TextRuns& runs);

// Sets item-level Formatting when every run agrees on the flags the item
// level models; mixed-format text keeps item formatting unset and is
// described run by run in the item's inline spans instead.
void set_uniform_formatting(const TextRuns& runs, docv1::TextItemBase* base);

// Sets the item-level hyperlink from the first link among the runs. Every
// link, this one included, also reaches the item as an InlineSpan carrying
// its own range, so nothing here has to record the rest.
void apply_run_hyperlinks(const TextRuns& runs, docv1::TextItemBase* base);

// Appends one InlineSpan per coalesced run: adjacent runs agreeing on
// every character attribute become one span whose range is code points
// into the item's own text. Runs carrying nothing worth recording add no
// span. owner_ref, when non-empty, registers each cross-reference span
// with the anchor index for resolution against the document's named
// anchors. base_offset is where the first run starts in the item's text,
// for items assembled from several run sequences. language is the
// document's own tag, so a run only carries one when it differs.
void add_run_spans(const TextRuns& runs, InlineSpans* spans,
                   const std::string& language, const std::string& owner_ref,
                   long long base_offset, AnchorIndex* anchors);

}  // namespace grparse::office_fold
