#pragma once

// A strict reader for one JSON text (RFC 8259).
//
// It exists for JSON a source document holds as content (a JSON-LD script
// in a web page), which gRParse decodes into typed messages. It keeps what
// that decoding needs and a generic JSON tree loses: the order of an
// object's members, a member name the source repeats, and whether a number
// was written as an integer. JSON a peer speaks on the wire is not read
// with this: it has a proto message and protobuf's own parser.
//
// The reader is strict because its input is untrusted and its output is
// presented as what the source says: text that is not JSON by the RFC is
// refused whole, with the offset and the reason, never half read. Nesting
// and the number of values are bounded, so a hostile block costs a bounded
// stack and a bounded heap.
//
// In include/ only so its tests and the fuzz target reach it; nothing outside
// gRParse is meant to use it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace grparse::json {

struct Member;

// One JSON value. Only the field its kind names is meaningful.
struct Value {
  enum class Kind { kNull, kBoolean, kInteger, kNumber, kString, kArray, kObject };
  Kind kind = Kind::kNull;
  bool boolean = false;
  // A number written with no fraction and no exponent that fits 64 bits.
  int64_t integer = 0;
  // Any other number.
  double number = 0.0;
  // UTF-8, escapes resolved.
  std::string string;
  std::vector<Value> elements;
  // In source order; a name the source repeats appears once per time.
  std::vector<Member> members;

  Value();
  Value(const Value&);
  Value(Value&&) noexcept;
  Value& operator=(const Value&);
  Value& operator=(Value&&) noexcept;
  ~Value();

  // The first member with this name, or nullptr. Only for objects.
  const Value* find(std::string_view name) const;
};

struct Member {
  std::string name;
  Value value;
};

struct Limits {
  // Arrays and objects open at once. The reader recurses once per level.
  size_t max_depth = 64;
  // Values in the whole text, containers included.
  size_t max_values = 100000;
};

struct Parsed {
  // True when the text is exactly one JSON value within the limits.
  bool ok = false;
  Value value;
  // When not ok: what is wrong, in words, and the byte offset it was found
  // at.
  std::string error;
  size_t offset = 0;
};

// Reads `text` as one JSON value, with only JSON whitespace around it.
// The text must be valid UTF-8.
Parsed parse(std::string_view text, const Limits& limits = {});

}  // namespace grparse::json
