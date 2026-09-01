// The stringification of the meta fields that have no Markdown rendering of
// their own: the export falls through to the model object's own printed
// form. Internal to the export renderers; include/grparse/document_render.h
// stays the only public surface.
#ifndef GRPARSE_RENDER_META_REPR_H
#define GRPARSE_RENDER_META_REPR_H

#include <string>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse::render {

// "name=value" pairs in field-declaration order with every value repr'd, the
// extra members last. Empty when the field drops from the export entirely:
// an unrepresentable language code, an entities field with no mentions.
std::string language_meta_repr(
    const ai::pipestream::document::v1::LanguageMetaField& meta);

std::string entities_meta_repr(
    const ai::pipestream::document::v1::EntitiesMetaField& meta);

std::string code_meta_repr(const ai::pipestream::document::v1::CodeMetaField& meta);

}  // namespace grparse::render

#endif
