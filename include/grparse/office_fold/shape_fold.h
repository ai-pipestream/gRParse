// The drawing planes: an Impress deck's slides and their shapes, and a
// Draw page's shapes. Both stream page-local geometry and both nest shapes
// in groups, so one fold holds the group nesting of each.
#pragma once

#include <map>
#include <string>
#include <tuple>
#include <utility>

#include "grparse/office_fold/fold_base.h"
#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

class ChartFold;
class ObjectFold;

// The laid-out box of a shape, page-local as both planes report it.
struct ShapeBox {
  double l = 0;
  double t = 0;
  double r = 0;
  double b = 0;
};

class ShapeFold : public FoldBase {
 public:
  ShapeFold(DocumentArena& arena, AnchorIndex& anchors)
      : FoldBase(arena, anchors) {}

  void on_slide(const officev1::Slide& slide);
  // An OLE2 shape whose embedded chart arrived ahead of the slide walk is
  // that chart's place in the slide's reading order, so the slide walk asks
  // the chart fold for it.
  // Any other embedded object (a spreadsheet, a formula, an OLE picture)
  // arrives ahead of the slide walk the same way and is placed by its OLE2
  // shape too.
  void on_slide_shape(const officev1::SlideShape& shape, ChartFold& charts,
                      ObjectFold& objects);
  void on_drawing_shape(const officev1::DrawingShape& shape);

  // True once a Slide event has mapped this ordinal.
  bool has_slide(int index) const { return slide_group_.contains(index); }
  // The slide's group, or the body when no Slide event for it arrived.
  std::string slide_group_ref(int index) const;

  // The picture a slide's graphic shape stands for, when the shape at this
  // position on this slide made one: the slide's EmbeddedImage fills it in
  // rather than adding the same picture a second time. Empty when none.
  std::string take_slide_picture(int slide_index, long long x, long long y);

  // Slide shapes stream in paint order, which is not where they sit on the
  // slide; once the deck is in, each slide's children are put in reading
  // order: top to bottom, and left to right along a row.
  void order_slides();

 private:
  // A shape with no text of its own that still stands for something drawn:
  // a picture, an object, a table or a media frame.
  void add_placeholder_picture(const officev1::SlideShape& shape,
                               const std::string& parent,
                               docv1::ContentLayer layer, int prov_page,
                               const ShapeBox& box);
  // Outline placeholders become one list item per non-blank paragraph,
  // whatever its depth: an outline is the slide's bullet list, and its
  // top-level lines are bullets, not section headings.
  void add_outline_paragraphs(const officev1::SlideShape& shape,
                              const std::string& parent,
                              docv1::ContentLayer layer, int prov_page,
                              const ShapeBox& box);
  // Every other text shape becomes one item holding the shape's paragraphs
  // joined by newlines. The deck's title is the first slide's title
  // placeholder; every other slide title heads a section of the deck.
  void add_shape_text(const officev1::SlideShape& shape,
                      const std::string& parent, docv1::ContentLayer layer,
                      int prov_page, const ShapeBox& box);

  std::map<int, std::string> slide_group_;
  // The first slide's ordinal, the only slide whose title is the deck's.
  int first_slide_ = -1;
  // Graphic shapes' pictures awaiting their EmbeddedImage: (slide, x, y) to
  // the picture's ref.
  std::map<std::tuple<int, long long, long long>, std::string>
      slide_pictures_;
  // Draw group nesting: (page index, child group_path) to the group's ref,
  // so a shape attaches under the group its group_path names.
  std::map<std::pair<int, std::string>, std::string> draw_groups_;
  // True once the first slide's title placeholder has become the deck's
  // title; a second title placeholder there is a section heading.
  bool deck_title_emitted_ = false;
};

}  // namespace grparse::office_fold
