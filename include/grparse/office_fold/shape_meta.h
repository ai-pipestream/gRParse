// A shape's identity on whichever item it became, and the alt text a
// picture carries.
#pragma once

#include <string>

#include "grparse/office_fold/fold_common.h"

namespace grparse::office_fold {

// Every field is optional, so a shape that names nothing leaves the message
// empty rather than claiming defaults.
void set_shape_meta(const std::string& shape_type, const std::string& name,
                    docv1::ShapeMeta* out);

// A drawing shape's identity, including the paint order and the rotation
// the office core reports in hundredths of a degree.
void set_drawing_shape_meta(const officev1::DrawingShape& shape,
                            docv1::ShapeMeta* out);

// Attaches a shape's alt text to a picture. Title and description are two
// source strings and have a slot each, so neither has to stand in for the
// other.
void set_alt_text(const std::string& title, const std::string& description,
                  docv1::PictureItem* picture);

}  // namespace grparse::office_fold
