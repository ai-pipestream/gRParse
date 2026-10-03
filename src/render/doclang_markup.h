// The DocLang markup walk behind render_doclang and render_dclx: the two
// exports differ only in whether the root declares the namespace and in
// where a picture's uri comes from, so both hand those in here. Internal to
// src/render.
#ifndef GRPARSE_RENDER_DOCLANG_MARKUP_H
#define GRPARSE_RENDER_DOCLANG_MARKUP_H

#include <functional>
#include <string>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse::render {

// The uri a rendered picture element states; empty writes no uri. Called
// once per picture the walk renders, in body order.
using PictureUri = std::function<std::string(const ai::pipestream::document::v1::PictureItem&)>;

std::string render_doclang_markup(const ai::pipestream::document::v1::Document& document,
                                  bool include_namespace, const PictureUri& picture_uri);

}  // namespace grparse::render

#endif
