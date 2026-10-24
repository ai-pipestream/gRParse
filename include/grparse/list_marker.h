#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace grparse {

// A list item's leading marker split off its text, the reference's
// ListItemMarkerProcessor.process_list_item rule (docling-ibm-models
// list_item_normalizer): a bullet glyph or a numbering form at the very
// start of the raw text, one whitespace character, then at least one more
// character. `marker` is the glyph or numbering verbatim ("-", "•", "1.",
// "(a)"), `text` is everything after the single separator (leading spaces
// beyond it are kept, as the reference keeps them), `enumerated` is true
// for a numbering form, and `prefix_code_points` is the length of
// marker plus separator in code points, so callers can shift spans that
// index into the text.
struct ListMarkerSplit {
  std::string marker;
  std::string text;
  bool enumerated = false;
  size_t prefix_code_points = 0;
};

// The split of `orig`, or nothing when it does not open with a marker
// followed by whitespace and content. Bullets are tried before numbering,
// and within numbering the compound forms ("1.2.3", "9a.") before the
// simple ones, first match wins, as in the reference.
std::optional<ListMarkerSplit> split_list_marker(std::string_view orig);

// Whether `token`, a whole word with nothing after it, opens another item
// of the list that `lead` opened: both are the same one of split_list_marker's
// bullet glyphs, both are form check boxes (U+2610..U+2612), or both are its
// numbering forms with one shape ("1." and "2.", "a)" and "b)", "iv." and
// "v."). A text layer's word cells carry a marker as its own word, so this
// tells where an item starts inside one visual line, and a decimal or an
// initial in an item's text ("1.5", "J.") is no marker of a bulleted or
// "1)" list.
bool is_sibling_list_marker(std::string_view lead, std::string_view token);

}  // namespace grparse
