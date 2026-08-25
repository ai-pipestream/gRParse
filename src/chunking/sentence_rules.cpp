#include "sentence_rules.h"

#include "token_counter.h"

namespace grparse::chunking {
namespace {

bool is_terminator(char32_t code_point) {
  return code_point == U'.' || code_point == U'!' || code_point == U'?' ||
         code_point == 0x2026;
}

bool is_closer(char32_t code_point) {
  return code_point == U'\'' || code_point == U'"' || code_point == U')' ||
         code_point == U']';
}

}  // namespace

std::vector<Span> split_sentences(const std::vector<char32_t>& points) {
  std::vector<Span> spans;
  const std::size_t size = points.size();
  std::size_t start = 0;
  std::size_t index = 0;
  while (index < size) {
    if (!is_terminator(points[index])) {
      ++index;
      continue;
    }
    std::size_t after = index + 1;
    while (after < size && is_closer(points[after])) ++after;
    if (after < size && !is_wordish_whitespace(points[after])) {
      // A terminator glued to more text (a decimal point, a URL, an
      // abbreviation the rule deliberately does not know) is not a boundary.
      index = after;
      continue;
    }
    // The whitespace run after the boundary rides with the sentence that
    // ends here, so the spans tile the input without gaps.
    std::size_t next = after;
    while (next < size && is_wordish_whitespace(points[next])) ++next;
    spans.push_back({start, next});
    start = next;
    index = next;
  }
  if (start < size) spans.push_back({start, size});
  return spans;
}

}  // namespace grparse::chunking
