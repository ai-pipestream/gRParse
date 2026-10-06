// The text-document plane: body paragraphs and tables, footnotes, headers
// and footers, indexes, text frames, draw-page shapes and the pictures
// anchored among the prose. It owns the reading-order bookkeeping a Writer
// document needs: the empty paragraphs an inline picture takes the place
// of, and the pictures that met none.
#pragma once

#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "grparse/office_fold/fold_base.h"
#include "grparse/office_fold/fold_common.h"
#include "grparse/office_fold/shape_fold.h"

namespace grparse::office_fold {

class WriterFold : public FoldBase {
 public:
  WriterFold(DocumentArena& arena, AnchorIndex& anchors,
             ShapeFold& shapes)
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
  // in reading order is left where the fold put it. Text frames and
  // shapes, which stream after all body text, are placed the same way.
  void anchor_trailing_pictures();

  // A page header or footer belongs to the page style that declares it and
  // so sits on every page laid out in that style. Once the pages are in,
  // each header and footer line becomes a furniture item with one
  // provenance entry per such page; a block whose style lays out no page
  // shows on no page and is left out.
  void place_headers_footers();

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

  // A frame, shape or picture anchored in a page header or footer streams
  // once per page that repeats it, every copy identical, anchor included.
  // True when this event repeats one already placed; the first copy then
  // moves to the furniture, since its one anchor places none of the
  // copies.
  bool repeats_placed_object(const std::string& key, const std::string& ref);

  // Floating pictures with a page but no box go before the first body item
  // of their page.
  void place_page_only_pictures();
  // One block's non-blank lines as furniture items placed on these pages.
  void add_header_footer(const officev1::HeaderFooter& block,
                         const std::vector<int>& pages);

  ShapeFold& shapes_;
  // Placed frames, shapes and pictures by their event's identity, to
  // recognise the per-page copies of a header's object.
  std::map<std::string, std::string> placed_objects_;
  std::set<std::string> repeated_objects_;
  // Objects anchored in a header or footer, by identity without their
  // place: the first copy goes to the furniture, later copies are dropped.
  std::set<std::string> header_objects_;
  // Caret extents (start y, end y, document twips) of the body tables.
  std::vector<std::pair<long long, long long>> table_spans_;
  // True once a text frame streamed: the body walk is over, and a table
  // arriving now is held by a frame.
  bool frames_seen_ = false;
  // Header and footer blocks, held until the pages are in.
  std::vector<officev1::HeaderFooter> header_footer_blocks_;
  // Writer draw-page group nesting: child group_path to the group's ref.
  // The text document has a single draw page, so the path alone keys it.
  std::map<std::string, std::string> writer_groups_;
  std::vector<ParagraphSlot> paragraph_slots_;
  // Text paragraphs by their start caret (page, x, y): a picture anchored
  // to a paragraph carries exactly that caret as its anchor.
  std::map<std::tuple<int, long long, long long>, std::string>
      paragraph_starts_;
  // Body pictures whose anchor met no empty paragraph, and text frames and
  // shapes, which stream after the body text: judged against the finished
  // body by anchor_trailing_pictures.
  std::set<std::string> floating_items_;
};

}  // namespace grparse::office_fold
