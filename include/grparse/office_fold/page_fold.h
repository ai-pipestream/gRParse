// The document plane: what the stream says about the document as a whole
// (its identity, its metadata) and about its pages (rectangles, renders,
// the page style catalogue).
#pragma once

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
};

}  // namespace grparse::office_fold
