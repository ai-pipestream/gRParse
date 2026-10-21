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

}  // namespace grparse
