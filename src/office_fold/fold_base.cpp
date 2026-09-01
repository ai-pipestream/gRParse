#include "grparse/office_fold/fold_base.h"

#include "grparse/office_fold/run_text.h"

namespace grparse::office_fold {

std::string FoldBase::fill_from_runs(const TextRuns& runs,
                                     const TextHandle& handle) {
  std::string text = concat_runs(runs);
  handle.base->set_text(text);
  handle.base->set_orig(text);
  set_uniform_formatting(runs, handle.base);
  add_run_spans(runs, handle.base->mutable_spans(), arena_.language(),
                handle.ref, 0, &anchors_);
  apply_run_hyperlinks(runs, handle.base);
  return text;
}

void FoldBase::add_spans(const TextRuns& runs, const TextHandle& handle,
                         long long base_offset) {
  add_run_spans(runs, handle.base->mutable_spans(), arena_.language(),
                handle.ref, base_offset, &anchors_);
}

}  // namespace grparse::office_fold
