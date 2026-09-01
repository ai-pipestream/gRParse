#include "grparse/office_fold/shape_fold.h"

#include <string>

#include "grparse/office_fold/chart_fold.h"
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

// Text is what the runs say, not that runs exist: an object shape's empty
// run is a placeholder, and a placeholder text item is nothing.
bool has_shape_text(const officev1::SlideShape& shape) {
  for (const officev1::SlideTextParagraph& paragraph : shape.paragraphs()) {
    if (runs_length(paragraph.runs()) > 0) return true;
  }
  return false;
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
}

void ShapeFold::add_placeholder_picture(const officev1::SlideShape& shape,
                                        const std::string& parent,
                                        docv1::ContentLayer layer,
                                        int prov_page, const ShapeBox& box) {
  docv1::PictureItem* picture = arena_.add_picture(
      docv1::DOC_ITEM_LABEL_PICTURE, layer, parent, nullptr);
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
    if (paragraph.runs().empty()) continue;
    TextHandle handle;
    if (paragraph.outline_depth() == 0) {
      handle = arena_.add_text(TextKind::kSectionHeader,
                               docv1::DOC_ITEM_LABEL_SECTION_HEADER, layer,
                               parent);
      handle.item->mutable_section_header()->set_level(1);
    } else {
      handle = arena_.add_text(TextKind::kList, docv1::DOC_ITEM_LABEL_LIST_ITEM,
                               layer, parent);
    }
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
    if (!deck_title_emitted_) {
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
                               ChartFold& charts) {
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
