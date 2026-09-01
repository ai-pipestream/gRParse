// How a text entry's class and label are reconstructed from the wire: the
// model rebuilds a class from the item's dedicated arm, or, for the generic
// arm foreign producers use, from the label alone. Internal to the export
// renderers; include/grparse/document_render.h stays the only public
// surface.
#ifndef GRPARSE_RENDER_TEXT_CLASS_H
#define GRPARSE_RENDER_TEXT_CLASS_H

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse::render {

enum class TextClass {
  kTitle,
  kSectionHeader,
  kListItem,
  kCode,
  kFormula,
  kFieldHeading,
  kFieldValue,
  kText,
};

TextClass classify_text(const ai::pipestream::document::v1::BaseTextItem& item);

// The label vocabulary the export includes. Everything outside it is dropped
// (its captions still render, exactly as in the reference).
bool exported_label(ai::pipestream::document::v1::DocItemLabel label);

// The label the model reconstructs: an unset tag falls back to the class
// default, which the export vocabulary always contains.
ai::pipestream::document::v1::DocItemLabel effective_label(
    ai::pipestream::document::v1::DocItemLabel label,
    ai::pipestream::document::v1::DocItemLabel fallback);

}  // namespace grparse::render

#endif
