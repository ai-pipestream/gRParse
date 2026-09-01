// Character-level helpers for the wiki storage dialect: case folding,
// whitespace, code-point counting and entity decoding. Internal to the
// storage handler; include/grparse/confluence_storage.h stays the only
// public surface.
#ifndef GRPARSE_CONFLUENCE_STORAGE_TEXT_H
#define GRPARSE_CONFLUENCE_STORAGE_TEXT_H

#include <cstdint>
#include <string>
#include <string_view>

namespace grparse::confluence {

std::string lowercase(std::string value);

std::string uppercase(std::string value);

bool ends_with(std::string_view value, std::string_view suffix);

bool is_space(char letter);

bool blank(std::string_view text);

std::string_view trim(std::string_view text);

// Characters, not bytes: the character spans the Document plane carries are
// counted in code points everywhere else, so they are here too.
long long code_points(std::string_view text);

// Appends one code point as UTF-8. Lone surrogates and out-of-range values
// have no encoding; the caller keeps the reference verbatim instead of
// letting a guess through.
void append_code_point(std::uint32_t code_point, std::string* out);

// Resolves the entity references the dialect uses: the five XML predefined
// ones, the numeric forms, and the non-breaking space that authoring tools
// emit constantly. Anything else stays verbatim, because a reference this
// parser does not know is still the author's text.
std::string decode_entities(std::string_view raw);

}  // namespace grparse::confluence

#endif
