#include "grparse/office_fold/object_fold.h"

#include <cstdlib>

#include "grparse/office_fold/shape_fold.h"
#include "grparse/office_fold/value_convert.h"

namespace grparse::office_fold {

void ObjectFold::add_formula(const officev1::EmbeddedObject& object,
                             const ObjectBox& box, const std::string& parent,
                             docv1::ContentLayer layer) {
  TextHandle handle = arena_.add_text(
      TextKind::kFormula, docv1::DOC_ITEM_LABEL_FORMULA, layer, parent);
  handle.base->set_text(object.formula());
  handle.base->set_orig(object.formula());
  attachments_.register_object(object, handle.ref);
  arena_.add_prov(handle.base->mutable_prov(), object.page_index(),
                  box.page_local, box.l, box.t, box.r, box.b, 0,
                  static_cast<long long>(object.formula().size()));
}

void ObjectFold::add_inner_table(const officev1::EmbeddedObject& object,
                                 const ObjectBox& box,
                                 const std::string& parent,
                                 docv1::ContentLayer layer) {
  std::string table_ref;
  docv1::TableItem* item = arena_.add_table(layer, parent, &table_ref);
  arena_.fold_table(object.inner_table(), item);
  attachments_.register_object(object, table_ref);
  arena_.add_prov(item->mutable_prov(), object.page_index(), box.page_local,
                  box.l, box.t, box.r, box.b, 0, 0);
}

void ObjectFold::add_object_picture(const officev1::EmbeddedObject& object,
                                    const ObjectBox& box,
                                    const std::string& parent,
                                    docv1::ContentLayer layer) {
  std::string picture_ref;
  docv1::PictureItem* picture = arena_.add_picture(
      docv1::DOC_ITEM_LABEL_PICTURE, layer, parent, &picture_ref);
  if (!object.name().empty()) picture->mutable_shape()->set_name(object.name());
  attachments_.register_object(object, picture_ref);
  if (!object.replacement_image().empty()) {
    docv1::ImageRef* ref = picture->mutable_image();
    ref->set_mimetype(object.replacement_mime_type());
    ref->mutable_size()->set_width(static_cast<double>(object.width_twips()));
    ref->mutable_size()->set_height(static_cast<double>(object.height_twips()));
    ref->set_uri(data_uri(object.replacement_mime_type(),
                          object.replacement_image()));
  }
  arena_.add_prov(picture->mutable_prov(), object.page_index(), box.page_local,
                  box.l, box.t, box.r, box.b, 0, 0);
}

void ObjectFold::emit(const officev1::EmbeddedObject& object,
                      const std::string& parent, docv1::ContentLayer layer) {
  ObjectBox box;
  box.page_local = !object.has_anchor();
  if (object.has_anchor()) {
    box.l = static_cast<double>(object.anchor().x());
    box.t = static_cast<double>(object.anchor().y());
  } else {
    box.l = static_cast<double>(object.position().x());
    box.t = static_cast<double>(object.position().y());
  }
  box.r = box.l + static_cast<double>(object.width_twips());
  box.b = box.t + static_cast<double>(object.height_twips());

  switch (object.kind()) {
    case officev1::EMBEDDED_OBJECT_KIND_FORMULA:
      return add_formula(object, box, parent, layer);
    case officev1::EMBEDDED_OBJECT_KIND_SPREADSHEET:
      return add_inner_table(object, box, parent, layer);
    default:
      return add_object_picture(object, box, parent, layer);
  }
}

void ObjectFold::on_embedded_object(const officev1::EmbeddedObject& object) {
  if (object.kind() == officev1::EMBEDDED_OBJECT_KIND_CHART) {
    // Sheet and slide charts wait for the event that places them in the
    // reading order (the sheet's SheetChart, the slide's OLE2 shape); a
    // Writer chart is placed by its caret anchor as it arrives.
    if (arena_.document_type() == "spreadsheet"
        || arena_.document_type() == "presentation") {
      return charts_.hold(object);
    }
    const bool page_local = !object.has_anchor();
    const officev1::TwipsPoint& at =
        page_local ? object.position() : object.anchor();
    const double l = static_cast<double>(at.x());
    const double t = static_cast<double>(at.y());
    const bool header_object = object.in_header_footer();
    return charts_.emit(&object, nullptr, header_object ? "#/furniture" : "#/body",
                        header_object ? docv1::CONTENT_LAYER_FURNITURE
                                      : docv1::CONTENT_LAYER_BODY,
                        page_local, object.page_index(), l, t,
                        l + static_cast<double>(object.width_twips()),
                        t + static_cast<double>(object.height_twips()));
  }
  // A deck streams its objects ahead of the slides; each waits for the
  // OLE2 shape that places it on its slide.
  if (arena_.document_type() == "presentation") {
    pending_[object.page_index()].push_back(object);
    return;
  }
  // An object anchored in a header or footer is page furniture.
  if (object.in_header_footer()) {
    emit(object, "#/furniture", docv1::CONTENT_LAYER_FURNITURE);
    return;
  }
  emit(object, "#/body", docv1::CONTENT_LAYER_BODY);
}

bool ObjectFold::take_pending(int page_index, const officev1::TwipsPoint& at,
                              const std::string& parent,
                              docv1::ContentLayer layer) {
  auto found = pending_.find(page_index);
  if (found == pending_.end()) return false;
  std::deque<officev1::EmbeddedObject>& waiting = found->second;
  for (auto it = waiting.begin(); it != waiting.end(); ++it) {
    // Positions come from the same model geometry on both events; a twip
    // of slack covers unit rounding.
    if (std::llabs(it->position().x() - at.x()) <= 1 &&
        std::llabs(it->position().y() - at.y()) <= 1) {
      const officev1::EmbeddedObject object = std::move(*it);
      waiting.erase(it);
      if (waiting.empty()) pending_.erase(found);
      emit(object, parent, layer);
      return true;
    }
  }
  return false;
}

void ObjectFold::flush(const ShapeFold& shapes) {
  std::map<int, std::deque<officev1::EmbeddedObject>> waiting;
  waiting.swap(pending_);
  for (auto& [page_index, objects] : waiting) {
    // No shape claimed them, but they still sit on their slide.
    const std::string parent = shapes.slide_group_ref(page_index);
    for (const officev1::EmbeddedObject& object : objects) {
      emit(object, parent, docv1::CONTENT_LAYER_BODY);
    }
  }
}

}  // namespace grparse::office_fold
