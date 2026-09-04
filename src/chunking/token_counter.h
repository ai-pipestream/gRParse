// The chunkers' token counters: the built-in deterministic rule set
// "wordish/1" and the HuggingFace-backed "hf/1". Internal seam of the
// chunking module (src/chunking/*); the service is the only caller besides
// the tests, and the chunking module has no public install surface.
//
// wordish/1 is the default because determinism is the contract: the same
// bytes must count to the same number forever, on every machine, with no
// model file in sight. It is a fixed, fully specified rule over Unicode code
// points:
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
//
// hf/1 counts with a real HuggingFace tokenizer.json through tokenizers-cpp
// (the Rust tokenizers crate behind a C ABI), for callers that need the
// budget measured in the embedding model's own units. The tokenizer file is
// a model, and models are this repo's sanctioned on-disk exception; it is
// resolved in this order:
//
//   1. the request's tokenizer_path option,
//   2. $GRPARSE_CHUNK_TOKENIZER,
//   3. $GRPARSE_MODELS_DIR/chunk/tokenizer.json ("/models" when the variable
//      is unset, the same default server_config applies).
//
// The file's own "padding" and "truncation" members are stripped before the
// load: a chunking counter measures the text it is given, and the
// fixed-length padding some published tokenizer.json files ship (docling.rs
// hit this with all-MiniLM-L6-v2) would report the padded length instead of
// the count. Special tokens are never added to the count.
//
// Threading: tokenizers-cpp hands out a mutable handle (its decode cache
// lives inside the instance), so a TokenCounter is never shared across
// threads; each chunk_hybrid call constructs its own. Chunking is not a hot
// path, and the per-call load is the price of never having to think about
// concurrent access.
#ifndef GRPARSE_CHUNKING_TOKEN_COUNTER_H
#define GRPARSE_CHUNKING_TOKEN_COUNTER_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <grpcpp/support/status.h>

namespace tokenizers {
class Tokenizer;
}

namespace grparse::chunking {

// The rule-set identifiers validate_hybrid_options accepts, as they appear
// on the wire in a chunk's rules_digest and in a request's tokenizer field.
inline constexpr std::string_view kTokenizerRules = "wordish/1";
inline constexpr std::string_view kHfTokenizerRules = "hf/1";

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

// Resolves the tokenizer.json an hf/1 request counts with, in the order
// documented above. `per_request_path` is the request's tokenizer_path
// option; pass an empty string when the request did not set one.
std::string resolve_hf_tokenizer_path(std::string_view per_request_path);

// Reads the tokenizer.json at `path` into `json_out` with the top-level
// "padding" and "truncation" members removed (see the header comment). A
// file that is unreadable or is not one well-formed JSON object is an
// INVALID_ARGUMENT naming the file.
grpc::Status load_hf_tokenizer_json(std::string_view path, std::string* json_out);

// The boundary-deciding counter of the hybrid chunker. The default
// constructor is wordish/1; TokenCounter::huggingface loads hf/1. Copies are
// disabled because the hf/1 state is an FFI handle.
class TokenCounter {
 public:
  // wordish/1. Out of line like the destructor: the unique_ptr member's
  // pointee is incomplete here.
  TokenCounter();
  ~TokenCounter();
  TokenCounter(TokenCounter&&) noexcept;
  TokenCounter& operator=(TokenCounter&&) noexcept;
  TokenCounter(const TokenCounter&) = delete;
  TokenCounter& operator=(const TokenCounter&) = delete;

  // Loads the tokenizer.json at `path` (already resolved, see
  // resolve_hf_tokenizer_path) into `out`. A file that does not load is an
  // INVALID_ARGUMENT naming it; the patched tokenizers-cpp reports a parse
  // failure as a null handle rather than aborting the process.
  static grpc::Status huggingface(std::string_view path, TokenCounter* out);

  // The rule-set identifier this counter counts under, for the digest.
  std::string_view rules() const { return rules_; }

  // True for wordish/1, whose at-most-one-token-per-code-point bound is what
  // the oversized split's hard cut by code point count relies on. A
  // byte-level hf/1 tokenizer can spend several tokens on one code point, so
  // the hf/1 cut measures instead.
  bool codepoint_bounded() const { return hf_ == nullptr; }

  int count(std::string_view text) const;
  int count(const char32_t* begin, const char32_t* end) const;

 private:
  std::string_view rules_ = kTokenizerRules;
  std::unique_ptr<tokenizers::Tokenizer> hf_;
};

}  // namespace grparse::chunking

#endif
