#include "grparse/office_fold/object_fold.h"

#include "grparse/office_fold/value_convert.h"

namespace grparse::office_fold {

void ObjectFold::add_formula(const officev1::EmbeddedObject& object,
                             const ObjectBox& box) {
  TextHandle handle =
      arena_.add_text(TextKind::kFormula, docv1::DOC_ITEM_LABEL_FORMULA,
                      docv1::CONTENT_LAYER_BODY, "#/body");
  handle.base->set_text(object.formula());
  handle.base->set_orig(object.formula());
  attachments_.register_object(object, handle.ref);
  arena_.add_prov(handle.base->mutable_prov(), object.page_index(),
                  box.page_local, box.l, box.t, box.r, box.b, 0,
                  static_cast<long long>(object.formula().size()));
}

void ObjectFold::add_inner_table(const officev1::EmbeddedObject& object,
                                 const ObjectBox& box) {
  std::string table_ref;
  docv1::TableItem* item =
      arena_.add_table(docv1::CONTENT_LAYER_BODY, "#/body", &table_ref);
  arena_.fold_table(object.inner_table(), item);
  attachments_.register_object(object, table_ref);
  arena_.add_prov(item->mutable_prov(), object.page_index(), box.page_local,
                  box.l, box.t, box.r, box.b, 0, 0);
}

void ObjectFold::add_object_picture(const officev1::EmbeddedObject& object,
                                    const ObjectBox& box) {
  std::string picture_ref;
  docv1::PictureItem* picture = arena_.add_picture(
      docv1::DOC_ITEM_LABEL_PICTURE, docv1::CONTENT_LAYER_BODY, "#/body",
      &picture_ref);
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

void ObjectFold::on_embedded_object(const officev1::EmbeddedObject& object) {
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
      return add_formula(object, box);
    case officev1::EMBEDDED_OBJECT_KIND_SPREADSHEET:
      return add_inner_table(object, box);
    case officev1::EMBEDDED_OBJECT_KIND_CHART:
      // Sheet and slide charts wait for the event that places them in the
      // reading order (the sheet's SheetChart, the slide's OLE2 shape); a
      // Writer chart is placed by its caret anchor as it arrives.
      if (arena_.document_type() == "spreadsheet"
          || arena_.document_type() == "presentation") {
        return charts_.hold(object);
      }
      return charts_.emit(&object, nullptr, "#/body", docv1::CONTENT_LAYER_BODY,
                          box.page_local, object.page_index(), box.l, box.t,
                          box.r, box.b);
    default:
      return add_object_picture(object, box);
  }
}

}  // namespace grparse::office_fold
