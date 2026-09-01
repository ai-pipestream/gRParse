#include "grparse/office_fold/shape_meta.h"

namespace grparse::office_fold {

void set_shape_meta(const std::string& shape_type, const std::string& name,
                    docv1::ShapeMeta* out) {
  if (!shape_type.empty()) out->set_shape_type(shape_type);
  if (!name.empty()) out->set_name(name);
}

void set_drawing_shape_meta(const officev1::DrawingShape& shape,
                            docv1::ShapeMeta* out) {
  set_shape_meta(shape.shape_type(), shape.name(), out);
  out->set_z_order(shape.z_order());
  if (shape.rotation() != 0) {
    out->set_rotation_degrees(static_cast<double>(shape.rotation()) / 100.0);
  }
}

void set_alt_text(const std::string& title, const std::string& description,
                  docv1::PictureItem* picture) {
  if (!description.empty()) {
    picture->mutable_meta()->mutable_description()->set_text(description);
  }
  if (!title.empty()) {
    picture->mutable_meta()->set_accessibility_title(title);
  }
}

}  // namespace grparse::office_fold
