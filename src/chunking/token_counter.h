// The chunkers' token counter, rule set "wordish/1". Internal seam of the
// chunking module (src/chunking/*); include/grparse/document_chunk.h stays
// the only public surface.
//
// Determinism is the contract: the same bytes must count to the same number
// forever, on every machine. That rules out a downloaded tokenizer model, a
// locale-sensitive classifier, and ICU. What remains is a fixed, fully
// specified rule over Unicode code points:
//
//   1. Decode the input as UTF-8 into code points. A malformed byte is one
//      code point of its own (U+FFFD in effect) and never merges runs.
//   2. Whitespace separates and counts nothing.
//   3. A maximal run of alphanumeric code points counts as ONE token, which
//      is the word rule for alphabetic scripts.
//   4. A CJK ideograph or a kana code point counts as one token by itself
//      and never joins a run, which is the word rule for scripts that do not
//      space their words.
//   5. Every other non-whitespace code point (punctuation, symbols, and any
//      script outside the built-in tables) counts as one token by itself.
//
// Rule 5 is deliberate and has a visible consequence: text in a script the
// table below does not cover (Arabic, Hebrew, Devanagari, Thai, ...) counts
// one token per code point rather than one per word. That over-counts, which
// is the safe direction for a budget, and it stays exactly reproducible.
// Widening the tables would change chunk boundaries, so it must arrive as a
// new rule version ("wordish/2"), never as an edit to this one.
#ifndef GRPARSE_CHUNKING_TOKEN_COUNTER_H
#define GRPARSE_CHUNKING_TOKEN_COUNTER_H

#include <cstdint>
#include <string_view>
#include <vector>

namespace grparse::chunking {

// The rule-set identifier this counter implements, as it appears on the wire
// in a chunk's rules_digest and in a request's tokenizer field.
inline constexpr std::string_view kTokenizerRules = "wordish/1";

// Decodes UTF-8 into code points. Any byte that cannot start or continue a
// well-formed sequence becomes U+FFFD, so the decode never loses position
// and never depends on the platform's locale.
std::vector<char32_t> decode_utf8(std::string_view text);

// Re-encodes a code point range as UTF-8.
std::string encode_utf8(const char32_t* begin, const char32_t* end);

// Number of code points in `text`, counted the same way decode_utf8 counts.
std::size_t codepoint_length(std::string_view text);

// The "wordish/1" token count of `text`. See the rules above.
int count_tokens(std::string_view text);

// The same count over already-decoded code points, for callers that decode
// once and measure many candidate spans.
int count_tokens(const char32_t* begin, const char32_t* end);

// True for the code points rule 2 treats as separators: the ASCII controls
// and space, NBSP, the Unicode space marks, and the line/paragraph
// separators.
bool is_wordish_whitespace(char32_t code_point);

}  // namespace grparse::chunking

#endif
