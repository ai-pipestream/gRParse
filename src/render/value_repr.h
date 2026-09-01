// The host-language value stringification the Markdown export inherits: the
// repr of a JSON payload, the number spelling, the truthiness guard, and the
// numeric-column test the table formatter aligns by. Internal to the export
// renderers; include/grparse/document_render.h stays the only public surface.
#ifndef GRPARSE_RENDER_VALUE_REPR_H
#define GRPARSE_RENDER_VALUE_REPR_H

#include <string>
#include <vector>

#include <google/protobuf/struct.pb.h>

namespace grparse::render {

// A quoted string literal: the quote is chosen the way repr() chooses it, and
// the control, format and separator code points are spelled out as escapes.
std::string python_string_repr(const std::string& text);

// The number as the model spells it, integral values narrowed to integers.
std::string number_text(double number);

// repr() of one JSON value: containers nest with repr'd members, struct keys
// render in byte order so an unordered wire map exports deterministically.
std::string value_repr(const google::protobuf::Value& value);

// str() of one JSON value; it differs from repr() only for a bare string.
std::string value_str(const google::protobuf::Value& value);

// A meta field renders only when its value is truthy, matching the
// reference's `str(value or "")` guard.
bool value_is_truthy(const google::protobuf::Value& value);

// A column is right-aligned when every value folds to a number; the fold
// starts at the boolean rung, so an all-empty column never does.
bool column_is_numeric(const std::vector<std::string>& values);

}  // namespace grparse::render

#endif
