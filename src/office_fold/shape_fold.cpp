#include "grparse/office_fold/shape_fold.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "grparse/document_geometry.h"
#include "grparse/office_fold/chart_fold.h"
#include "grparse/office_fold/object_fold.h"
#include "grparse/office_fold/run_text.h"
#include "grparse/office_fold/shape_meta.h"
#include "grparse/office_fold/value_convert.h"

namespace grparse::office_fold {

namespace {

// The path a child of this group carries: the parent's path with the
// group's own paint order appended.
std::string child_group_path(const std::string& group_path, int z_order) {
  return group_path.empty()
             ? std::to_string(z_order)
             : group_path + "/" + std::to_string(z_order);
}

bool blank_paragraph(const officev1::SlideTextParagraph& paragraph) {
  return blank_text(concat_runs(paragraph.runs()));
}

// Text is what the runs say, not that runs exist: an object shape's empty
// run is a placeholder, a run of spaces or line breaks lays out nothing, and
// a text item of either is nothing.
bool has_shape_text(const officev1::SlideShape& shape) {
  return !std::ranges::all_of(shape.paragraphs(), blank_paragraph);
}

// A shape type that stands for something drawn even with no text.
bool drawn_shape_type(const std::string& shape_type) {
  return ends_with(shape_type, "GraphicObjectShape")
      || ends_with(shape_type, "OLE2Shape")
      || ends_with(shape_type, "TableShape")
      || ends_with(shape_type, "MediaShape");
}

}  // namespace

std::string ShapeFold::slide_group_ref(int index) const {
  auto found = slide_group_.find(index);
  return found != slide_group_.end() ? found->second : std::string("#/body");
}

void ShapeFold::on_slide(const officev1::Slide& slide) {
  docv1::GroupItem* group =
      arena_.add_group("#/body", docv1::GROUP_LABEL_SLIDE, slide.name(),
                       docv1::CONTENT_LAYER_BODY);
  auto* fields = group->mutable_meta()->mutable_custom_fields();
  (*fields)["layout"] = num_value(slide.layout());
  if (!slide.master_page_name().empty()) {
    (*fields)["master_page_name"] = str_value(slide.master_page_name());
  }
  slide_group_[slide.index()] = group->self_ref();
  if (first_slide_ < 0 || slide.index() < first_slide_) {
    first_slide_ = slide.index();
  }
}

std::string ShapeFold::take_slide_picture(int slide_index, long long x,
                                          long long y) {
  // Both events read the same model position; a twip of slack covers unit
  // rounding.
  for (long long dx = -1; dx <= 1; dx++) {
    for (long long dy = -1; dy <= 1; dy++) {
      auto found = slide_pictures_.find({slide_index, x + dx, y + dy});
      if (found == slide_pictures_.end()) continue;
      std::string ref = std::move(found->second);
      slide_pictures_.erase(found);
      return ref;
    }
  }
  return std::string();
}

void ShapeFold::add_placeholder_picture(const officev1::SlideShape& shape,
                                        const std::string& parent,
                                        docv1::ContentLayer layer,
                                        int prov_page, const ShapeBox& box) {
  std::string picture_ref;
  docv1::PictureItem* picture = arena_.add_picture(
      docv1::DOC_ITEM_LABEL_PICTURE, layer, parent, &picture_ref);
  if (ends_with(shape.shape_type(), "GraphicObjectShape")) {
    slide_pictures_[{shape.slide_index(), shape.position().x(),
                     shape.position().y()}] = picture_ref;
  }
  docv1::ShapeMeta* shape_meta = picture->mutable_shape();
  set_shape_meta(shape.shape_type(), std::string(), shape_meta);
  shape_meta->set_z_order(shape.z_order());
  set_alt_text(shape.title(), shape.description(), picture);
  arena_.add_prov(picture->mutable_prov(), prov_page, true, box.l, box.t, box.r,
                  box.b, 0, 0);
}

void ShapeFold::add_outline_paragraphs(const officev1::SlideShape& shape,
                                       const std::string& parent,
                                       docv1::ContentLayer layer,
                                       int prov_page, const ShapeBox& box) {
  for (const officev1::SlideTextParagraph& paragraph : shape.paragraphs()) {
    if (blank_paragraph(paragraph)) continue;
    TextHandle handle = arena_.add_text(
        TextKind::kList, docv1::DOC_ITEM_LABEL_LIST_ITEM, layer, parent);
    fill_from_runs(paragraph.runs(), handle);
    set_shape_meta(shape.shape_type(), std::string(),
                   handle.base->mutable_shape());
    handle.base->mutable_shape()->set_z_order(shape.z_order());
    arena_.add_prov(handle.base->mutable_prov(), prov_page, true, box.l, box.t,
                    box.r, box.b, 0, runs_length(paragraph.runs()));
  }
}

void ShapeFold::add_shape_text(const officev1::SlideShape& shape,
                               const std::string& parent,
                               docv1::ContentLayer layer, int prov_page,
                               const ShapeBox& box) {
  TextHandle handle;
  if (shape.placeholder_role() == officev1::PLACEHOLDER_ROLE_TITLE
      && !shape.notes()) {
    if (!deck_title_emitted_ && shape.slide_index() == first_slide_) {
      handle = arena_.add_text(TextKind::kTitle, docv1::DOC_ITEM_LABEL_TITLE,
                               layer, parent);
      deck_title_emitted_ = true;
    } else {
      handle = arena_.add_text(TextKind::kSectionHeader,
                               docv1::DOC_ITEM_LABEL_SECTION_HEADER, layer,
                               parent);
      handle.item->mutable_section_header()->set_level(1);
    }
  } else {
    handle = arena_.add_text(TextKind::kText, docv1::DOC_ITEM_LABEL_TEXT, layer,
                             parent);
  }
  std::string text;
  long long length = 0;
  for (const officev1::SlideTextParagraph& paragraph : shape.paragraphs()) {
    if (blank_paragraph(paragraph)) continue;
    if (!text.empty()) {
      text += "\n";
      length += 1;
    }
    text += concat_runs(paragraph.runs());
    // Spans stay aligned with the joined text, newline separators included.
    add_spans(paragraph.runs(), handle, length);
    length += runs_length(paragraph.runs());
  }
  handle.base->set_text(text);
  handle.base->set_orig(text);
  set_shape_meta(shape.shape_type(), std::string(),
                 handle.base->mutable_shape());
  handle.base->mutable_shape()->set_z_order(shape.z_order());
  arena_.add_prov(handle.base->mutable_prov(), prov_page, true, box.l, box.t,
                  box.r, box.b, 0, length);
}

void ShapeFold::on_slide_shape(const officev1::SlideShape& shape,
                               ChartFold& charts, ObjectFold& objects) {
  if (shape.is_empty_placeholder()) return;
  const std::string parent = slide_group_ref(shape.slide_index());
  const docv1::ContentLayer layer = shape.notes() ? docv1::CONTENT_LAYER_NOTES
                                                  : docv1::CONTENT_LAYER_BODY;
  // Notes shapes carry no slide-page provenance: their geometry is in
  // notes-page space, which has no PageImage.
  const int prov_page = shape.notes() ? -1 : shape.slide_index();
  ShapeBox box;
  box.l = static_cast<double>(shape.position().x());
  box.t = static_cast<double>(shape.position().y());
  box.r = box.l + static_cast<double>(shape.width_twips());
  box.b = box.t + static_cast<double>(shape.height_twips());

  // A table shape carries its content in a cell grid, not in shape text, so
  // it folds into a real table under the slide rather than a placeholder.
  if (shape.has_table()) {
    docv1::TableItem* item = arena_.add_table(layer, parent, nullptr);
    arena_.fold_table(shape.table(), item);
    arena_.add_prov(item->mutable_prov(), prov_page, true, box.l, box.t, box.r,
                    box.b, 0, 0);
    return;
  }

  if (ends_with(shape.shape_type(), "OLE2Shape")) {
    officev1::EmbeddedObject object;
    if (charts.take_pending(shape.slide_index(), &shape.position(), &object)) {
      charts.emit(&object, nullptr, parent, layer, true, prov_page, box.l,
                  box.t, box.r, box.b);
      return;
    }
    if (objects.take_pending(shape.slide_index(), shape.position(), parent,
                             layer)) {
      return;
    }
  }

  if (!has_shape_text(shape)) {
    if (drawn_shape_type(shape.shape_type())) {
      add_placeholder_picture(shape, parent, layer, prov_page, box);
    }
    return;
  }

  if (shape.placeholder_role() == officev1::PLACEHOLDER_ROLE_OUTLINE) {
    add_outline_paragraphs(shape, parent, layer, prov_page, box);
    return;
  }
  add_shape_text(shape, parent, layer, prov_page, box);
}

void ShapeFold::order_slides() {
  docv1::Document& document = arena_.document();
  const std::map<int, double> heights = document_page_heights(document);
  struct Placed {
    docv1::RefItem ref;
    TopDownBox box;
  };
  for (const auto& [index, group_ref] : slide_group_) {
    docv1::GroupItem* group = arena_.group_by_ref(group_ref);
    std::vector<Placed> placed;
    std::vector<docv1::RefItem> unplaced;
    for (const docv1::RefItem& child : group->children()) {
      const std::optional<ItemPlacement> placement =
          item_placement(document, child.ref(), heights);
      if (placement.has_value()) {
        placed.push_back({child, placement->box});
      } else {
        unplaced.push_back(child);
      }
    }
    if (placed.size() < 2) continue;
    std::ranges::stable_sort(placed, {},
                             [](const Placed& p) { return p.box.top; });
    // Shapes on one row read left to right: a later-starting shape whose
    // top sits within half a line of its neighbour's, and which lies wholly
    // to its left, comes first.
    for (bool swapped = true; swapped;) {
      swapped = false;
      for (size_t i = 0; i + 1 < placed.size(); i++) {
        const TopDownBox& a = placed[i].box;
        const TopDownBox& b = placed[i + 1].box;
        const double half_line = 0.5 * std::min(a.height(), b.height());
        if (b.right <= a.left && b.top - a.top < half_line) {
          std::swap(placed[i], placed[i + 1]);
          swapped = true;
        }
      }
    }
    group->clear_children();
    for (Placed& p : placed) *group->add_children() = std::move(p.ref);
    // Items with no box on the slide (notes) keep their order after it.
    for (docv1::RefItem& ref : unplaced) {
      *group->add_children() = std::move(ref);
    }
  }
}

void ShapeFold::on_drawing_shape(const officev1::DrawingShape& shape) {
  std::string parent = "#/body";
  if (auto container =
          draw_groups_.find({shape.page_index(), shape.group_path()});
      container != draw_groups_.end()) {
    parent = container->second;
  }
  ShapeBox box;
  box.l = static_cast<double>(shape.position().x());
  box.t = static_cast<double>(shape.position().y());
  box.r = box.l + static_cast<double>(shape.width_twips());
  box.b = box.t + static_cast<double>(shape.height_twips());
  if (shape.is_group()) {
    docv1::GroupItem* group =
        arena_.add_group(parent, docv1::GROUP_LABEL_PICTURE_AREA, shape.name(),
                         docv1::CONTENT_LAYER_BODY);
    draw_groups_[{shape.page_index(),
                  child_group_path(shape.group_path(), shape.z_order())}] =
        group->self_ref();
    return;
  }
  if (shape.has_text()) {
    TextHandle handle = arena_.add_text(TextKind::kText,
                                        docv1::DOC_ITEM_LABEL_TEXT,
                                        docv1::CONTENT_LAYER_BODY, parent);
    fill_from_runs(shape.runs(), handle);
    set_drawing_shape_meta(shape, handle.base->mutable_shape());
    // Draw positions are page-local per part.
    arena_.add_prov(handle.base->mutable_prov(), shape.page_index(), true,
                    box.l, box.t, box.r, box.b, 0, runs_length(shape.runs()));
    return;
  }
  docv1::PictureItem* picture = arena_.add_picture(
      docv1::DOC_ITEM_LABEL_PICTURE, docv1::CONTENT_LAYER_BODY, parent,
      nullptr);
  set_drawing_shape_meta(shape, picture->mutable_shape());
  set_alt_text(shape.title(), shape.description(), picture);
  arena_.add_prov(picture->mutable_prov(), shape.page_index(), true, box.l,
                  box.t, box.r, box.b, 0, 0);
}

}  // namespace grparse::office_fold
