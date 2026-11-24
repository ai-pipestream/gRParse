#include "grparse/json_reader.h"

#include <charconv>
#include <cmath>
#include <system_error>
#include <utility>

#include <simdutf.h>

namespace grparse::json {

Value::Value() = default;
Value::Value(const Value&) = default;
Value::Value(Value&&) noexcept = default;
Value& Value::operator=(const Value&) = default;
Value& Value::operator=(Value&&) noexcept = default;
Value::~Value() = default;

const Value* Value::find(std::string_view name) const {
  for (const Member& member : members) {
    if (member.name == name) return &member.value;
  }
  return nullptr;
}

namespace {

class Reader {
 public:
  Reader(std::string_view text, const Limits& limits) : text_(text), limits_(limits) {}

  Parsed run() {
    Parsed parsed;
    if (!simdutf::validate_utf8(text_.data(), text_.size())) {
      parsed.error = "the text is not valid UTF-8";
      return parsed;
    }
    skip_whitespace();
    if (value(&parsed.value, 0)) {
      skip_whitespace();
      if (at_ != text_.size()) fail("text after the end of the JSON value");
    }
    if (!error_.empty()) {
      parsed.value = Value();
      parsed.error = std::move(error_);
      parsed.offset = error_at_;
      return parsed;
    }
    parsed.ok = true;
    return parsed;
  }

 private:
  bool fail(std::string why) {
    if (error_.empty()) {
      error_ = std::move(why);
      error_at_ = at_;
    }
    return false;
  }

  bool done() const { return at_ >= text_.size(); }
  char peek() const { return text_[at_]; }

  void skip_whitespace() {
    while (!done()) {
      const char c = peek();
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return;
      ++at_;
    }
  }

  bool literal(std::string_view word) {
    if (text_.substr(at_, word.size()) != word) return fail("not a JSON value");
    at_ += word.size();
    return true;
  }

  bool value(Value* out, size_t depth) {
    if (++values_ > limits_.max_values) {
      return fail("more than " + std::to_string(limits_.max_values) + " values");
    }
    if (done()) return fail("the text ends where a value is expected");
    switch (peek()) {
      case '{':
        return object(out, depth);
      case '[':
        return array(out, depth);
      case '"':
        out->kind = Value::Kind::kString;
        return string(&out->string);
      case 't':
        out->kind = Value::Kind::kBoolean;
        out->boolean = true;
        return literal("true");
      case 'f':
        out->kind = Value::Kind::kBoolean;
        out->boolean = false;
        return literal("false");
      case 'n':
        out->kind = Value::Kind::kNull;
        return literal("null");
      default:
        return number(out);
    }
  }

  bool open(size_t depth) {
    if (depth >= limits_.max_depth) {
      return fail("nested deeper than " + std::to_string(limits_.max_depth) + " levels");
    }
    ++at_;
    skip_whitespace();
    return true;
  }

  bool object(Value* out, size_t depth) {
    out->kind = Value::Kind::kObject;
    if (!open(depth)) return false;
    if (!done() && peek() == '}') {
      ++at_;
      return true;
    }
    while (true) {
      if (done() || peek() != '"') return fail("a member name is expected");
      Member member;
      if (!string(&member.name)) return false;
      skip_whitespace();
      if (done() || peek() != ':') return fail("':' is expected after a member name");
      ++at_;
      skip_whitespace();
      if (!value(&member.value, depth + 1)) return false;
      out->members.push_back(std::move(member));
      skip_whitespace();
      if (done()) return fail("the text ends inside an object");
      if (peek() == '}') {
        ++at_;
        return true;
      }
      if (peek() != ',') return fail("',' or '}' is expected in an object");
      ++at_;
      skip_whitespace();
    }
  }

  bool array(Value* out, size_t depth) {
    out->kind = Value::Kind::kArray;
    if (!open(depth)) return false;
    if (!done() && peek() == ']') {
      ++at_;
      return true;
    }
    while (true) {
      Value element;
      if (!value(&element, depth + 1)) return false;
      out->elements.push_back(std::move(element));
      skip_whitespace();
      if (done()) return fail("the text ends inside an array");
      if (peek() == ']') {
        ++at_;
        return true;
      }
      if (peek() != ',') return fail("',' or ']' is expected in an array");
      ++at_;
      skip_whitespace();
    }
  }

  static void append_utf8(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  // Four hex digits at the cursor.
  bool hex4(uint32_t* out) {
    if (text_.size() - at_ < 4) return fail("a \\u escape needs four hex digits");
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[at_ + static_cast<size_t>(i)];
      uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<uint32_t>(c - 'a') + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<uint32_t>(c - 'A') + 10;
      } else {
        return fail("a \\u escape needs four hex digits");
      }
      value = (value << 4) | digit;
    }
    at_ += 4;
    *out = value;
    return true;
  }

  bool escape(std::string* out) {
    if (done()) return fail("the text ends inside an escape");
    const char c = peek();
    ++at_;
    switch (c) {
      case '"':
      case '\\':
      case '/':
        out->push_back(c);
        return true;
      case 'b':
        out->push_back('\b');
        return true;
      case 'f':
        out->push_back('\f');
        return true;
      case 'n':
        out->push_back('\n');
        return true;
      case 'r':
        out->push_back('\r');
        return true;
      case 't':
        out->push_back('\t');
        return true;
      case 'u': {
        uint32_t unit = 0;
        if (!hex4(&unit)) return false;
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
          return fail("a low surrogate escape with no high surrogate before it");
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
          // A high surrogate names a character only with the low one that
          // follows it. A string cannot hold half a character.
          if (text_.substr(at_, 2) != "\\u") {
            return fail("a high surrogate escape with no low surrogate after it");
          }
          at_ += 2;
          uint32_t low = 0;
          if (!hex4(&low)) return false;
          if (low < 0xDC00 || low > 0xDFFF) {
            return fail("a high surrogate escape with no low surrogate after it");
          }
          unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        }
        append_utf8(unit, out);
        return true;
      }
      default:
        --at_;
        return fail("an escape JSON does not have");
    }
  }

  bool string(std::string* out) {
    ++at_;  // The opening quote.
    while (true) {
      const size_t run = at_;
      while (!done() && peek() != '"' && peek() != '\\' &&
             static_cast<unsigned char>(peek()) >= 0x20) {
        ++at_;
      }
      out->append(text_.data() + run, at_ - run);
      if (done()) return fail("the text ends inside a string");
      const char c = peek();
      if (c == '"') {
        ++at_;
        return true;
      }
      if (c == '\\') {
        ++at_;
        if (!escape(out)) return false;
        continue;
      }
      return fail("a control character inside a string");
    }
  }

  bool digits() {
    const size_t start = at_;
    while (!done() && peek() >= '0' && peek() <= '9') ++at_;
    return at_ != start;
  }

  bool number(Value* out) {
    const size_t start = at_;
    if (!done() && peek() == '-') ++at_;
    if (done() || peek() < '0' || peek() > '9') {
      at_ = start;
      return fail("not a JSON value");
    }
    if (peek() == '0') {
      ++at_;
      if (!done() && peek() >= '0' && peek() <= '9') {
        return fail("a number with a leading zero");
      }
    } else {
      digits();
    }
    bool integral = true;
    if (!done() && peek() == '.') {
      integral = false;
      ++at_;
      if (!digits()) return fail("a digit is expected after the decimal point");
    }
    if (!done() && (peek() == 'e' || peek() == 'E')) {
      integral = false;
      ++at_;
      if (!done() && (peek() == '+' || peek() == '-')) ++at_;
      if (!digits()) return fail("a digit is expected in the exponent");
    }
    const char* first = text_.data() + start;
    const char* last = text_.data() + at_;
    if (integral) {
      int64_t whole = 0;
      const auto [end, ec] = std::from_chars(first, last, whole);
      if (ec == std::errc() && end == last) {
        out->kind = Value::Kind::kInteger;
        out->integer = whole;
        return true;
      }
      // An integer too large for 64 bits is still a number: it is read as
      // the nearest double, as every JSON reader without big integers does.
    }
    double real = 0.0;
    const auto [end, ec] = std::from_chars(first, last, real);
    if (ec != std::errc() || end != last || !std::isfinite(real)) {
      at_ = start;
      return fail("a number outside the range of a double");
    }
    out->kind = Value::Kind::kNumber;
    out->number = real;
    return true;
  }

  std::string_view text_;
  const Limits& limits_;
  size_t at_ = 0;
  size_t values_ = 0;
  std::string error_;
  size_t error_at_ = 0;
};

}  // namespace

Parsed parse(std::string_view text, const Limits& limits) {
  return Reader(text, limits).run();
}

}  // namespace grparse::json
