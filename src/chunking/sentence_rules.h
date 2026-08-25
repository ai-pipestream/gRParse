// Sentence boundaries for the hybrid chunker's split pass, rule set
// "sentence/1". Internal seam of the chunking module (src/chunking/*).
//
// The rule is intentionally the smallest one that is exactly reproducible:
//
//   A boundary follows a terminator ('.', '!', '?', or U+2026 HORIZONTAL
//   ELLIPSIS), then any run of closing quotes and brackets from the set
//   ' " ) ], when what comes next is whitespace or the end of the text.
//
// There is NO abbreviation handling, no locale, no dictionary: "Dr. Who"
// splits after "Dr.". That is a deliberate trade. An abbreviation list is a
// language-specific, growing thing, and growing it would silently move chunk
// boundaries for documents that already chunked. Any change to the rule
// arrives as "sentence/2" instead.
//
// Spans are half-open ranges of code point indices and tile the input: the
// whitespace that follows a boundary belongs to the sentence that precedes
// it, so concatenating every span reproduces the input exactly.
#ifndef GRPARSE_CHUNKING_SENTENCE_RULES_H
#define GRPARSE_CHUNKING_SENTENCE_RULES_H

#include <cstddef>
#include <string_view>
#include <vector>

namespace grparse::chunking {

// The rule-set identifier this splitter implements, as it appears on the
// wire in a chunk's rules_digest.
inline constexpr std::string_view kSentenceRules = "sentence/1";

// A half-open [begin, end) range of code point indices.
struct Span {
  std::size_t begin = 0;
  std::size_t end = 0;

  bool empty() const { return begin >= end; }
  std::size_t size() const { return end - begin; }
};

// The sentence spans of `points`, in order, tiling [0, points.size()).
// Empty input yields no spans.
std::vector<Span> split_sentences(const std::vector<char32_t>& points);

}  // namespace grparse::chunking

#endif
