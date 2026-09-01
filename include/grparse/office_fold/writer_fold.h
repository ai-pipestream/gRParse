// The text-document plane: body paragraphs and tables, footnotes, headers
// and footers, indexes, text frames, draw-page shapes and the pictures
// anchored among the prose. It owns the reading-order bookkeeping a Writer
// document needs: the empty paragraphs an inline picture takes the place
// of, and the pictures that met none.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "grparse/office_fold/fold_base.h"
#include "grparse/office_fold/fold_common.h"
#include "grparse/office_fold/shape_fold.h"

namespace grparse::office_fold {

class WriterFold : public FoldBase {
 public:
  WriterFold(DocumentArena& arena, AnchorIndex& anchors,
             const ShapeFold& shapes)
      : FoldBase(arena, anchors), shapes_(shapes) {}

  void on_paragraph(const officev1::Paragraph& paragraph);
  void on_table(const officev1::TableData& table);
  void on_embedded_image(const officev1::EmbeddedImage& image);
  void on_footnote(const officev1::Footnote& footnote);
  void on_header_footer(const officev1::HeaderFooter& block);
  void on_document_index(const officev1::DocumentIndex& index);
  void on_text_frame(const officev1::TextFrame& frame);
  void on_shape(const officev1::Shape& shape);

  // A Writer picture whose anchor met no empty paragraph (a wrapped image
  // beside prose, a picture arriving after the paragraphs of its page) was
  // appended to the body in arrival order, wherever its page is: a page-23
  // figure after page 208's last paragraph. Once every paragraph is in,
  // such a picture is placed by its provenance the way the CV enrichment's
  // pictures are: after the paragraph beside it, in page order. Only a
  // picture the fold could not slot AND that sits behind a body item which
  // comes later on the page plane is trailing; a slotted picture keeps the
  // place its anchor paragraph gave it, and an unslotted one that arrived
  // in reading order is left where the fold put it.
  void anchor_trailing_pictures();

 private:
  // Where an empty Writer paragraph sat in the body: an inline picture's
  // anchor paragraph is exactly such a paragraph, so the picture takes its
  // place in the reading order instead of trailing the body.
  struct ParagraphSlot {
    int page_index = -1;
    long long caret_y = 0;
    // The body child the slot follows; empty when it led the body.
    std::string after_ref;
  };

  // A paragraph with nothing but whitespace is no text item: it is either a
  // spacer or the anchor line of an inline picture, and in the latter case
  // the picture takes the slot when it arrives. True when the paragraph was
  // such a slot.
  bool record_empty_paragraph(const officev1::Paragraph& paragraph,
                              const std::string& text);
  // The body item a paragraph becomes, by its style, outline level and list
  // level.
  TextHandle add_paragraph_item(const officev1::Paragraph& paragraph);
  // Gives an inline picture the place of the empty paragraph it is anchored
  // in, or remembers that it met none.
  void slot_inline_picture(const officev1::EmbeddedImage& image,
                           const std::string& picture_ref);
  // The slot of the empty paragraph an inline picture is anchored in: same
  // page, caret at or below the picture's top and within its height plus a
  // line. -1 when no slot fits.
  int take_anchor_slot(int page_index, long long anchor_y, long long height);

  const ShapeFold& shapes_;
  // Writer draw-page group nesting: child group_path to the group's ref.
  // The text document has a single draw page, so the path alone keys it.
  std::map<std::string, std::string> writer_groups_;
  std::vector<ParagraphSlot> paragraph_slots_;
  // Body pictures whose anchor met no empty paragraph, judged against the
  // finished body by anchor_trailing_pictures.
  std::set<std::string> unslotted_pictures_;
};

}  // namespace grparse::office_fold
