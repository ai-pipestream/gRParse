// Internal seams of the canonical JSON export (src/render/canonical_json_*):
// the reference dump's number and string formatting rules and a streaming
// writer that reproduces its two-space pretty printing exactly. Not part of
// the public API; include/grparse/document_render.h stays the only public
// surface.
#ifndef GRPARSE_RENDER_CANONICAL_JSON_WRITER_H
#define GRPARSE_RENDER_CANONICAL_JSON_WRITER_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace grparse::render {

// Formats a double exactly as the reference serializer does: shortest
// round-trip digits, fixed notation while the decimal point position stays
// in (-4, 16], otherwise scientific as d[.ddd]e{+|-}NN with a two-digit
// minimum exponent. Integral values keep a trailing ".0" in fixed notation
// ("2.0", "-0.0"); non-finite values render as NaN / Infinity / -Infinity.
std::string canonical_double(double value);

// Formats an integral double as its exact integer decimal, however large
// ("1e23" becomes "99999999999999991611392", not a rounded power of ten).
// The value must satisfy std::floor(value) == value and be finite.
std::string canonical_integral_decimal(double value);

// Escapes a UTF-8 string for a double-quoted JSON literal with ASCII-only
// output: the two specials and control characters use their short escapes,
// everything outside 0x20..0x7E becomes lowercase \uXXXX (surrogate pairs
// above the BMP). Returns the escaped body without the surrounding quotes;
// malformed UTF-8 bytes degrade to U+FFFD.
std::string escape_json_ascii(std::string_view text);

// Appends the same escape to an existing buffer, with a bulk-copy fast path
// for runs that need no escaping (the overwhelmingly common case: data URIs
// and plain text). escape_json_ascii is a thin wrapper over this.
void escape_json_ascii_into(std::string& out, std::string_view text);

// Streaming JSON writer with the exact layout of a two-space indented
// pretty printer: every element on its own line, ": " after keys, empty
// containers collapsed to [] and {}. Keys and values are emitted strictly
// in call order; the caller owns all ordering decisions.
class JsonWriter {
 public:
  void begin_object() { begin_container('{'); }
  void end_object() { end_container('}'); }
  void begin_array() { begin_container('['); }
  void end_array() { end_container(']'); }

  // Writes the key of the next object member.
  void key(std::string_view name);

  void value_string(std::string_view text);
  void value_bool(bool value);
  void value_null();
  void value_int(std::int64_t value);
  void value_uint(std::uint64_t value);
  void value_double(double value);
  // Writes preformatted token text (a number already rendered).
  void value_raw(std::string_view token);

  // Convenience for the ubiquitous key/value pairs.
  void member_string(std::string_view name, std::string_view text) {
    key(name);
    value_string(text);
  }
  void member_bool(std::string_view name, bool value) {
    key(name);
    value_bool(value);
  }
  void member_int(std::string_view name, std::int64_t value) {
    key(name);
    value_int(value);
  }
  void member_double(std::string_view name, double value) {
    key(name);
    value_double(value);
  }

  // Pre-sizes the output buffer; callers with a size estimate (e.g. the
  // source message's byte size) avoid repeated growth of large documents.
  void reserve(std::size_t bytes) { out_.reserve(bytes); }

  // The finished document. Valid once every container has been closed.
  const std::string& str() const { return out_; }
  std::string take() { return std::move(out_); }

 private:
  void begin_container(char open);
  void end_container(char close);
  // Positions the cursor for the next array element: separator, newline,
  // indent. A no-op at the top level.
  void next_element();
  // Positions the cursor for a value: directly after a key it goes in
  // place, otherwise it is a new array element.
  void position_value();

  std::string out_;
  // One flag per open container: true until its first element is written.
  std::vector<bool> empty_stack_;
  // True between key() and the value that completes the member.
  bool pending_value_ = false;
};

}  // namespace grparse::render

#endif
