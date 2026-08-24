#include "canonical_json_writer.h"

#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>

namespace grparse::render {

namespace {

// Splits the shortest round-trip representation of a finite double into its
// digit string and decimal point position (the value is 0.D1D2... * 10^decpt).
struct ShortestDigits {
  bool negative = false;
  std::string digits;  // no leading or trailing zeros beyond significance
  int decpt = 0;
};

ShortestDigits shortest_digits(double value) {
  // std::to_chars scientific yields the shortest digit sequence that
  // round-trips; the layout is re-derived from it rather than trusted.
  std::array<char, 64> buf;
  auto [end, ec] =
      std::to_chars(buf.data(), buf.data() + buf.size(), value, std::chars_format::scientific);
  assert(ec == std::errc());
  std::string_view text(buf.data(), static_cast<std::size_t>(end - buf.data()));

  ShortestDigits out;
  std::size_t pos = 0;
  if (pos < text.size() && text[pos] == '-') {
    out.negative = true;
    ++pos;
  }
  while (pos < text.size() && text[pos] != 'e') {
    if (text[pos] != '.') out.digits.push_back(text[pos]);
    ++pos;
  }
  int exponent = 0;
  if (pos < text.size()) {
    ++pos;  // 'e'
    bool exp_negative = false;
    if (text[pos] == '+' || text[pos] == '-') {
      exp_negative = text[pos] == '-';
      ++pos;
    }
    for (; pos < text.size(); ++pos) exponent = exponent * 10 + (text[pos] - '0');
    if (exp_negative) exponent = -exponent;
  }
  // Scientific notation puts one digit before the point: d.ddd * 10^e means
  // the decimal point sits after position e + 1 of the digit string.
  out.decpt = exponent + 1;
  return out;
}

void append_two_digit_exponent(std::string* out, int exponent) {
  out->push_back('e');
  out->push_back(exponent < 0 ? '-' : '+');
  const int magnitude = exponent < 0 ? -exponent : exponent;
  char digits[16];
  int n = std::snprintf(digits, sizeof digits, "%02d", magnitude);
  out->append(digits, static_cast<std::size_t>(n));
}

}  // namespace

std::string canonical_double(double value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
  if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";

  const ShortestDigits parts = shortest_digits(value);
  const std::string& d = parts.digits;
  const int n = static_cast<int>(d.size());
  std::string out;
  if (parts.negative) out.push_back('-');

  // Fixed notation while the decimal point position stays within (-4, 16]
  // digits of the front; scientific otherwise, with a two-digit minimum
  // exponent. This is the reference formatter's switch point.
  if (parts.decpt > 16 || parts.decpt < -3) {
    out.append(d, 0, 1);
    if (n > 1) {
      out.push_back('.');
      out.append(d, 1, std::string::npos);
    }
    append_two_digit_exponent(&out, parts.decpt - 1);
  } else if (parts.decpt <= 0) {
    out.append("0.");
    out.append(static_cast<std::size_t>(-parts.decpt), '0');
    out.append(d);
  } else if (parts.decpt >= n) {
    out.append(d);
    out.append(static_cast<std::size_t>(parts.decpt - n), '0');
    out.append(".0");
  } else {
    out.append(d, 0, static_cast<std::size_t>(parts.decpt));
    out.push_back('.');
    out.append(d, static_cast<std::size_t>(parts.decpt), std::string::npos);
  }
  return out;
}

std::string canonical_integral_decimal(double value) {
  assert(std::isfinite(value) && std::floor(value) == value);
  if (value == 0.0) return "0";  // covers -0.0, which the dump prints as 0

  // Exact decimal of mantissa * 2^exponent via a base-1e9 accumulator, so
  // huge integral doubles print their exact value, not a rounded shortest
  // form.
  int exponent = 0;
  double mantissa = std::frexp(std::fabs(value), &exponent);
  std::uint64_t bits = 0;
  // 53 doublings turn the [0.5, 1) fraction into an exact 53-bit integer.
  for (int i = 0; i < 53; ++i) mantissa *= 2.0;
  bits = static_cast<std::uint64_t>(mantissa);
  exponent -= 53;
  while (exponent < 0) {  // integral values only trim trailing zero bits
    bits >>= 1;
    ++exponent;
  }

  constexpr std::uint32_t kBase = 1000000000;
  std::vector<std::uint32_t> limbs;  // little-endian base-1e9
  limbs.push_back(static_cast<std::uint32_t>(bits % kBase));
  bits /= kBase;
  while (bits != 0) {
    limbs.push_back(static_cast<std::uint32_t>(bits % kBase));
    bits /= kBase;
  }
  for (int i = 0; i < exponent; ++i) {
    std::uint64_t carry = 0;
    for (auto& limb : limbs) {
      const std::uint64_t doubled = 2ull * limb + carry;
      limb = static_cast<std::uint32_t>(doubled % kBase);
      carry = doubled / kBase;
    }
    if (carry != 0) limbs.push_back(static_cast<std::uint32_t>(carry));
  }

  std::string out;
  if (value < 0) out.push_back('-');
  char chunk[16];
  std::snprintf(chunk, sizeof chunk, "%u", limbs.back());
  out.append(chunk);
  for (auto it = limbs.rbegin() + 1; it != limbs.rend(); ++it) {
    std::snprintf(chunk, sizeof chunk, "%09u", *it);
    out.append(chunk);
  }
  return out;
}

void escape_json_ascii_into(std::string& out, std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  const auto append_u16 = [&out](unsigned code) {
    char buf[6] = {'\\', 'u', kHex[(code >> 12) & 0xf], kHex[(code >> 8) & 0xf],
                   kHex[(code >> 4) & 0xf], kHex[code & 0xf]};
    out.append(buf, 6);
  };
  const auto plain = [](unsigned char byte) {
    return byte >= 0x20 && byte <= 0x7e && byte != '"' && byte != '\\';
  };
  std::size_t i = 0;
  while (i < text.size()) {
    // Bulk-copy the maximal run of bytes that pass through unescaped.
    std::size_t run = i;
    while (run < text.size() &&
           plain(static_cast<unsigned char>(text[run]))) {
      ++run;
    }
    if (run != i) {
      out.append(text, i, run - i);
      i = run;
      if (i >= text.size()) break;
    }
    const unsigned char byte = static_cast<unsigned char>(text[i]);
    if (byte == '"') {
      out.append("\\\"");
      ++i;
    } else if (byte == '\\') {
      out.append("\\\\");
      ++i;
    } else if (byte < 0x80) {
      switch (byte) {
        case '\b': out.append("\\b"); break;
        case '\t': out.append("\\t"); break;
        case '\n': out.append("\\n"); break;
        case '\f': out.append("\\f"); break;
        case '\r': out.append("\\r"); break;
        default: append_u16(byte); break;
      }
      ++i;
    } else {
      // Decode one UTF-8 sequence; malformed input degrades to U+FFFD per
      // offending byte so the output stays valid ASCII JSON.
      unsigned code = 0xfffd;
      std::size_t length = 1;
      const auto continuation = [&text](std::size_t at) {
        return at < text.size() &&
               (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80;
      };
      if ((byte & 0xe0) == 0xc0 && continuation(i + 1)) {
        code = (byte & 0x1fu) << 6 |
               (static_cast<unsigned char>(text[i + 1]) & 0x3fu);
        length = code >= 0x80 ? 2 : 1;
        if (length == 1) code = 0xfffd;
      } else if ((byte & 0xf0) == 0xe0 && continuation(i + 1) && continuation(i + 2)) {
        code = (byte & 0x0fu) << 12 |
               (static_cast<unsigned char>(text[i + 1]) & 0x3fu) << 6 |
               (static_cast<unsigned char>(text[i + 2]) & 0x3fu);
        length = code >= 0x800 && (code < 0xd800 || code > 0xdfff) ? 3 : 1;
        if (length == 1) code = 0xfffd;
      } else if ((byte & 0xf8) == 0xf0 && continuation(i + 1) && continuation(i + 2) &&
                 continuation(i + 3)) {
        code = (byte & 0x07u) << 18 |
               (static_cast<unsigned char>(text[i + 1]) & 0x3fu) << 12 |
               (static_cast<unsigned char>(text[i + 2]) & 0x3fu) << 6 |
               (static_cast<unsigned char>(text[i + 3]) & 0x3fu);
        length = code >= 0x10000 && code <= 0x10ffff ? 4 : 1;
        if (length == 1) code = 0xfffd;
      }
      if (code >= 0x10000) {
        const unsigned offset = code - 0x10000;
        append_u16(0xd800 + (offset >> 10));
        append_u16(0xdc00 + (offset & 0x3ff));
      } else {
        append_u16(code);
      }
      i += length;
    }
  }
}

std::string escape_json_ascii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  escape_json_ascii_into(out, text);
  return out;
}

void JsonWriter::begin_container(char open) {
  position_value();
  out_.push_back(open);
  empty_stack_.push_back(true);
}

void JsonWriter::end_container(char close) {
  const bool was_empty = empty_stack_.back();
  empty_stack_.pop_back();
  if (!was_empty) {
    out_.push_back('\n');
    out_.append(2 * empty_stack_.size(), ' ');
  }
  out_.push_back(close);
}

void JsonWriter::next_element() {
  if (empty_stack_.empty()) return;  // top-level single value
  if (!empty_stack_.back()) out_.push_back(',');
  empty_stack_.back() = false;
  out_.push_back('\n');
  out_.append(2 * empty_stack_.size(), ' ');
}

void JsonWriter::position_value() {
  if (pending_value_) {
    pending_value_ = false;
    return;
  }
  next_element();
}

void JsonWriter::key(std::string_view name) {
  next_element();
  out_.push_back('"');
  escape_json_ascii_into(out_, name);
  out_.append("\": ");
  pending_value_ = true;
}

void JsonWriter::value_string(std::string_view text) {
  position_value();
  out_.push_back('"');
  escape_json_ascii_into(out_, text);
  out_.push_back('"');
}

void JsonWriter::value_bool(bool value) {
  position_value();
  out_.append(value ? "true" : "false");
}

void JsonWriter::value_null() {
  position_value();
  out_.append("null");
}

void JsonWriter::value_int(std::int64_t value) {
  position_value();
  out_.append(std::to_string(value));
}

void JsonWriter::value_uint(std::uint64_t value) {
  position_value();
  out_.append(std::to_string(value));
}

void JsonWriter::value_double(double value) {
  position_value();
  out_.append(canonical_double(value));
}

void JsonWriter::value_raw(std::string_view token) {
  position_value();
  out_.append(token);
}

}  // namespace grparse::render
