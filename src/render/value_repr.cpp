#include "value_repr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/struct.pb.h>

#include "canonical_json_writer.h"

namespace grparse::render {
namespace {

// ---------------------------------------------------------------------------
// Numeric classification, as the reference's table formatter performs it to
// pick a column's alignment. Mirrors Python's int()/float() acceptance:
// surrounding whitespace, an optional sign, digit-group underscores, and the
// literal infinity and not-a-number spellings.
// ---------------------------------------------------------------------------

// Digits with at most single underscores between them, never at either end.
bool scan_digits(std::string_view text, std::size_t* at) {
  bool saw_digit = false;
  bool prev_underscore = false;
  const std::size_t start = *at;
  while (*at < text.size()) {
    const char c = text[*at];
    if (c >= '0' && c <= '9') {
      saw_digit = true;
      prev_underscore = false;
    } else if (c == '_') {
      if (!saw_digit || prev_underscore) return false;
      prev_underscore = true;
    } else {
      break;
    }
    ++*at;
  }
  if (prev_underscore) return false;
  if (!saw_digit) *at = start;
  return saw_digit;
}

bool matches_keyword(std::string_view text, std::size_t* at,
                     std::string_view word) {
  if (text.size() - *at < word.size()) return false;
  for (std::size_t i = 0; i < word.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[*at + i])) != word[i]) {
      return false;
    }
  }
  *at += word.size();
  return true;
}

std::string_view python_trimmed(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\n\r\f\v");
  if (begin == std::string_view::npos) return std::string_view();
  return text.substr(begin, text.find_last_not_of(" \t\n\r\f\v") - begin + 1);
}

bool is_python_int(std::string_view raw) {
  const std::string_view text = python_trimmed(raw);
  std::size_t at = 0;
  if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
  if (!scan_digits(text, &at)) return false;
  return at == text.size();
}

// Whether Python's float() would accept the text, and what it would produce.
bool is_python_float(std::string_view raw, double* value) {
  const std::string_view text = python_trimmed(raw);
  std::size_t at = 0;
  bool negative = false;
  if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
    negative = text[at] == '-';
    ++at;
  }
  if (matches_keyword(text, &at, "infinity") || matches_keyword(text, &at, "inf")) {
    if (at != text.size()) return false;
    *value = negative ? -HUGE_VAL : HUGE_VAL;
    return true;
  }
  if (matches_keyword(text, &at, "nan")) {
    if (at != text.size()) return false;
    *value = std::nan("");
    return true;
  }
  const std::size_t mantissa_start = at;
  const bool leading_digits = scan_digits(text, &at);
  bool fraction_digits = false;
  if (at < text.size() && text[at] == '.') {
    ++at;
    fraction_digits = scan_digits(text, &at);
  }
  if (!leading_digits && !fraction_digits) return false;
  if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
    ++at;
    if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
    if (!scan_digits(text, &at)) return false;
  }
  if (at != text.size()) return false;
  // Underscores are grouping only; strtod does not accept them.
  std::string plain;
  plain.reserve(text.size());
  if (negative) plain.push_back('-');
  for (std::size_t i = mantissa_start; i < text.size(); ++i) {
    if (text[i] != '_') plain.push_back(text[i]);
  }
  *value = std::strtod(plain.c_str(), nullptr);
  return true;
}

// The reference's numeric test: float-convertible, and not an overflow to an
// infinity or a not-a-number that the text did not spell out literally.
bool is_reference_number(const std::string& text) {
  double value = 0.0;
  if (!is_python_float(text, &value)) return false;
  if (!std::isinf(value) && !std::isnan(value)) return true;
  std::string lowered;
  lowered.reserve(text.size());
  for (const char c : text) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return lowered == "inf" || lowered == "-inf" || lowered == "nan";
}

// "^(([+-]?[0-9]{1,3})(?:,([0-9]{3}))*)?(?(1)\.[0-9]*|\.[0-9]+)?$"
bool has_thousands_separators(const std::string& text) {
  std::size_t at = 0;
  bool integer_part = false;
  const std::size_t sign_at = at;
  if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
  std::size_t digits = 0;
  while (at < text.size() && digits < 3 && std::isdigit(static_cast<unsigned char>(text[at]))) {
    ++at;
    ++digits;
  }
  if (digits == 0) {
    at = sign_at;
  } else {
    integer_part = true;
    while (at + 3 < text.size() && text[at] == ',') {
      if (!std::isdigit(static_cast<unsigned char>(text[at + 1])) ||
          !std::isdigit(static_cast<unsigned char>(text[at + 2])) ||
          !std::isdigit(static_cast<unsigned char>(text[at + 3]))) {
        break;
      }
      at += 4;
    }
  }
  if (at < text.size() && text[at] == '.') {
    ++at;
    std::size_t fraction = 0;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
      ++at;
      ++fraction;
    }
    if (!integer_part && fraction == 0) return false;
  }
  return at == text.size();
}

// The reference's type ladder for a cell value; only the ordering matters.
enum class ValueType { kNone = 0, kBool = 1, kInt = 2, kFloat = 3, kStr = 5 };

ValueType value_type(const std::string& text) {
  if (text.empty()) return ValueType::kNone;
  if (text == "True" || text == "False") return ValueType::kBool;
  if (is_python_int(text) ||
      (has_thousands_separators(text) && !text.contains('.'))) {
    return ValueType::kInt;
  }
  if (is_reference_number(text) || has_thousands_separators(text)) {
    return ValueType::kFloat;
  }
  return ValueType::kStr;
}

// ---------------------------------------------------------------------------
// Value stringification. Custom meta fields carry arbitrary JSON payloads,
// and the reference renders them with the host language's str(); these
// helpers reproduce that, including the repr() nesting inside containers.
// ---------------------------------------------------------------------------

// The code points the reference spells out inside a quoted string: the
// assigned control, format and separator categories, minus the plain space.
// The unassigned, private-use and surrogate code points it also spells out
// are deliberately not listed: that set moves with the character database
// version, and no document text reaches this path carrying one.
bool non_printable(char32_t code_point) {
  if (code_point < 0x20) return true;
  static constexpr std::pair<char32_t, char32_t> kRanges[] = {
      {0x007f, 0x00a0},   {0x00ad, 0x00ad},   {0x0600, 0x0605},
      {0x061c, 0x061c},   {0x06dd, 0x06dd},   {0x070f, 0x070f},
      {0x0890, 0x0891},   {0x08e2, 0x08e2},   {0x1680, 0x1680},
      {0x180e, 0x180e},   {0x2000, 0x200f},   {0x2028, 0x202f},
      {0x205f, 0x2064},   {0x2066, 0x206f},   {0x3000, 0x3000},
      {0xfeff, 0xfeff},   {0xfff9, 0xfffb},   {0x110bd, 0x110bd},
      {0x110cd, 0x110cd}, {0x13430, 0x1343f}, {0x1bca0, 0x1bca3},
      {0x1d173, 0x1d17a}, {0xe0001, 0xe0001}, {0xe0020, 0xe007f},
  };
  for (const auto& [low, high] : kRanges) {
    if (code_point < low) return false;
    if (code_point <= high) return true;
  }
  return false;
}

// The code point starting at `at` and the bytes it spans, or a span of 0 for
// a byte that starts no well-formed sequence (left verbatim).
std::pair<char32_t, std::size_t> utf8_code_point(std::string_view text,
                                                 std::size_t at) {
  const auto lead = static_cast<unsigned char>(text[at]);
  std::size_t span = 0;
  char32_t code_point = 0;
  if (lead < 0x80) return {lead, 1};
  if ((lead & 0xe0) == 0xc0) {
    span = 2;
    code_point = lead & 0x1f;
  } else if ((lead & 0xf0) == 0xe0) {
    span = 3;
    code_point = lead & 0x0f;
  } else if ((lead & 0xf8) == 0xf0) {
    span = 4;
    code_point = lead & 0x07;
  } else {
    return {0, 0};
  }
  if (at + span > text.size()) return {0, 0};
  for (std::size_t i = 1; i < span; ++i) {
    const auto byte = static_cast<unsigned char>(text[at + i]);
    if ((byte & 0xc0) != 0x80) return {0, 0};
    code_point = (code_point << 6) | (byte & 0x3f);
  }
  return {code_point, span};
}

}  // namespace

std::string python_string_repr(const std::string& text) {
  static constexpr char kHex[] = "0123456789abcdef";
  const auto append_hex = [](std::string* out, char32_t value, int digits) {
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
      out->push_back(kHex[(value >> shift) & 0xf]);
    }
  };
  const char quote = text.contains('\'') && !text.contains('"') ? '"' : '\'';
  std::string out(1, quote);
  for (std::size_t at = 0; at < text.size();) {
    const char c = text[at];
    if (c == quote || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
      ++at;
      continue;
    }
    if (c == '\n' || c == '\r' || c == '\t') {
      out.push_back('\\');
      out.push_back(c == '\n' ? 'n' : c == '\r' ? 'r' : 't');
      ++at;
      continue;
    }
    const auto [code_point, span] = utf8_code_point(text, at);
    if (span == 0) {
      out.push_back(c);
      ++at;
      continue;
    }
    if (!non_printable(code_point)) {
      out.append(text, at, span);
    } else if (code_point < 0x100) {
      out.append("\\x");
      append_hex(&out, code_point, 2);
    } else if (code_point < 0x10000) {
      out.append("\\u");
      append_hex(&out, code_point, 4);
    } else {
      out.append("\\U");
      append_hex(&out, code_point, 8);
    }
    at += span;
  }
  out.push_back(quote);
  return out;
}

std::string number_text(double number) {
  // The model narrows integral numbers to integers on import.
  if (std::isfinite(number) && std::floor(number) == number) {
    return canonical_integral_decimal(number);
  }
  return canonical_double(number);
}

std::string value_str(const google::protobuf::Value& value) {
  // str() differs from repr() only for a bare string.
  if (value.kind_case() == google::protobuf::Value::kStringValue) {
    return value.string_value();
  }
  return value_repr(value);
}

std::string value_repr(const google::protobuf::Value& value) {
  switch (value.kind_case()) {
    case google::protobuf::Value::kBoolValue:
      return value.bool_value() ? "True" : "False";
    case google::protobuf::Value::kNumberValue:
      return number_text(value.number_value());
    case google::protobuf::Value::kStringValue:
      return python_string_repr(value.string_value());
    case google::protobuf::Value::kListValue: {
      std::string out = "[";
      bool first = true;
      for (const auto& entry : value.list_value().values()) {
        if (!first) out.append(", ");
        first = false;
        out.append(value_repr(entry));
      }
      return out + "]";
    }
    case google::protobuf::Value::kStructValue: {
      // The wire map is unordered; sorted keys keep the rendering stable.
      std::vector<std::string> keys;
      keys.reserve(value.struct_value().fields().size());
      for (const auto& [key, unused_value] : value.struct_value().fields()) {
        keys.push_back(key);
      }
      std::ranges::sort(keys);
      std::string out = "{";
      bool first = true;
      for (const auto& key : keys) {
        if (!first) out.append(", ");
        first = false;
        out.append(python_string_repr(key));
        out.append(": ");
        out.append(value_repr(value.struct_value().fields().at(key)));
      }
      return out + "}";
    }
    case google::protobuf::Value::kNullValue:
    case google::protobuf::Value::KIND_NOT_SET: return "None";
  }
  return "None";
}

// ---------------------------------------------------------------------------

// A meta field renders only when its value is truthy, matching the
// reference's `str(value or "")` guard.
bool value_is_truthy(const google::protobuf::Value& value) {
  switch (value.kind_case()) {
    case google::protobuf::Value::kBoolValue: return value.bool_value();
    case google::protobuf::Value::kNumberValue: return value.number_value() != 0.0;
    case google::protobuf::Value::kStringValue: return !value.string_value().empty();
    case google::protobuf::Value::kListValue:
      return !value.list_value().values().empty();
    case google::protobuf::Value::kStructValue:
      return !value.struct_value().fields().empty();
    case google::protobuf::Value::kNullValue:
    case google::protobuf::Value::KIND_NOT_SET: return false;
  }
  return false;
}

// The reference's type ladder is folded per column: a column is right-aligned
// only when every value in it reaches the numeric rungs.
bool column_is_numeric(const std::vector<std::string>& values) {
  ValueType folded = ValueType::kBool;
  for (const auto& value : values) {
    folded = std::max(folded, value_type(value));
  }
  return folded == ValueType::kInt || folded == ValueType::kFloat;
}

}  // namespace grparse::render
