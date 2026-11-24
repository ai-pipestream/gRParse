// The document plane: what the stream says about the document as a whole
// (its identity, its metadata) and about its pages (rectangles, renders,
// the page style catalogue).
#pragma once

#include <map>
#include <vector>

#include "grparse/office_fold/arena.h"
#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

class PageFold {
 public:
  explicit PageFold(DocumentArena& arena) : arena_(arena) {}

  void on_document_info(const officev1::DocumentInfo& info);
  void on_page_image(const officev1::PageImage& image);
  void on_metadata(const officev1::DocumentMetadata& meta);
  void on_page_style(const officev1::PageStyleInfo& style);

  // A body paragraph, read for one thing only: whether it restarts page
  // numbering. A restart takes effect on the page its paragraph starts on,
  // and the first restart seen for a page is the one that page prints.
  void note_page_number_restart(const officev1::Paragraph& paragraph);

  // Labels pages with their printed numbers from the paragraphs that
  // restart page numbering: the restart page prints the paragraph's offset
  // and each later page one more, until the next restart. Pages before the
  // first restart print their ordinal, which PageItem.page_no already says,
  // so they keep no label.
  void resolve_page_labels();

  // Checks every page style name a page carries against the PageStyle
  // catalogue, which streams in after the page images do. A name that
  // matches nothing in a catalogue that was collected is kept, because it
  // is still what the layout reported, and named in a warning so the
  // divergence is visible rather than silent.
  void resolve_page_styles();

 private:
  DocumentArena& arena_;
  // The 1-based page numbers whose style name came off the wire, so the
  // catalogue check at the end of the stream visits only those.
  std::vector<int> styled_pages_;
  // The printed number each page-numbering restart begins at, by the
  // zero-based index of the page its paragraph starts on.
  std::map<int, int> page_number_restarts_;
};

}  // namespace grparse::office_fold
