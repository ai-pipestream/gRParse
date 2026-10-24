#include "grparse/list_marker.h"

#include <array>
#include <cstdint>

namespace grparse {
namespace {

constexpr size_t kNoMatch = std::string_view::npos;

// One code point at `at`: its value and its encoded length. A malformed
// lead or continuation byte reads as a one-byte U+FFFD, which matches no
// marker and no separator.
struct CodePoint {
  char32_t value = 0;
  size_t length = 0;
};

CodePoint decode_at(std::string_view text, size_t at) {
  const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
  const unsigned char lead = byte(at);
  size_t length = 0;
  char32_t value = 0;
  if (lead < 0x80) return {lead, 1};
  if ((lead & 0xE0) == 0xC0) {
    length = 2;
    value = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    value = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    value = lead & 0x07;
  } else {
    return {0xFFFD, 1};
  }
  if (at + length > text.size()) return {0xFFFD, 1};
  for (size_t i = 1; i < length; ++i) {
    if ((byte(at + i) & 0xC0) != 0x80) return {0xFFFD, 1};
    value = (value << 6) | (byte(at + i) & 0x3F);
  }
  const char32_t min_value = length == 2 ? 0x80U : (length == 3 ? 0x800U : 0x10000U);
  if (value < min_value || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
    return {0xFFFD, 1};
  }
  return {value, length};
}

// The reference's bullet character classes, all four patterns together:
// symbol bullets, common ASCII and Unicode bullets, arrows, check marks.
bool is_bullet(char32_t c) {
  static constexpr std::array<char32_t, 24> kBullets = {
      U'•', U'‣', U'◦', U'⁃', U'⁌', U'⁍', U'∙', U'▪',
      U'▫', U'●', U'○', U'-',      U'*',      U'+',      U'·', U'►',
      U'▶', U'▸', U'➤', U'➢', U'✓', U'✔', U'✗', U'✘'};
  for (const char32_t bullet : kBullets) {
    if (c == bullet) return true;
  }
  return false;
}

// Python's `\s` on str: the code points str.isspace() accepts.
bool is_space(char32_t c) {
  return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 ||
         c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
         c == 0x202F || c == 0x205F || c == 0x3000;
}

// Python's `\d` on str: every Unicode Decimal_Number (Nd) range, each ten
// consecutive code points starting at its zero digit.
bool is_unicode_digit(char32_t c) {
  static constexpr std::array<char32_t, 75> kDecimalZeros = {
      0x0030U,  0x0660U,  0x06F0U,  0x07C0U,  0x0966U,  0x09E6U,  0x0A66U,  0x0AE6U,  0x0B66U,
      0x0BE6U,  0x0C66U,  0x0CE6U,  0x0D66U,  0x0DE6U,  0x0E50U,  0x0ED0U,  0x0F20U,  0x1040U,
      0x1090U,  0x17E0U,  0x1810U,  0x1946U,  0x19D0U,  0x1A80U,  0x1A90U,  0x1B50U,  0x1BB0U,
      0x1C40U,  0x1C50U,  0xA620U,  0xA8D0U,  0xA900U,  0xA9D0U,  0xA9F0U,  0xAA50U,  0xABF0U,
      0xFF10U,  0x104A0U, 0x10D30U, 0x10D40U, 0x11066U, 0x110F0U, 0x11136U, 0x111D0U, 0x112F0U,
      0x11450U, 0x114D0U, 0x11650U, 0x116C0U, 0x116D0U, 0x116DAU, 0x11730U, 0x118E0U, 0x11950U,
      0x11BF0U, 0x11C50U, 0x11D50U, 0x11DA0U, 0x11F50U, 0x16130U, 0x16A60U, 0x16AC0U, 0x16B50U,
      0x1CCF0U, 0x1D7CEU, 0x1D7D8U, 0x1D7E2U, 0x1D7ECU, 0x1D7F6U, 0x1E140U, 0x1E2F0U, 0x1E4F0U,
      0x1E950U, 0x1ECE0U, 0x1FBF0U,
  };
  for (const char32_t zero : kDecimalZeros) {
    if (c < zero) return false;
    if (c <= zero + 9) return true;
  }
  return false;
}

size_t digit_length(std::string_view s, size_t at) {
  if (at >= s.size()) return 0;
  const CodePoint cp = decode_at(s, at);
  return is_unicode_digit(cp.value) ? cp.length : 0;
}

bool is_lower(std::string_view s, size_t at) { return at < s.size() && s[at] >= 'a' && s[at] <= 'z'; }
bool is_upper(std::string_view s, size_t at) { return at < s.size() && s[at] >= 'A' && s[at] <= 'Z'; }
bool is_letter(std::string_view s, size_t at) { return is_lower(s, at) || is_upper(s, at); }
bool is_char(std::string_view s, size_t at, char c) { return at < s.size() && s[at] == c; }

size_t digits(std::string_view s, size_t at) {
  const size_t first = digit_length(s, at);
  if (first == 0) return kNoMatch;
  at += first;
  while (const size_t len = digit_length(s, at)) at += len;
  return at;
}

size_t run_of(std::string_view s, size_t at, std::string_view set) {
  const size_t begin = at;
  while (at < s.size() && set.find(s[at]) != std::string_view::npos) ++at;
  return at == begin ? kNoMatch : at;
}

size_t then_char(std::string_view s, size_t at, char c) {
  return at != kNoMatch && is_char(s, at, c) ? at + 1 : kNoMatch;
}

// The reference's numbered patterns in its order, each returning the end
// of the marker or kNoMatch. Every pattern ends on a fixed character or a
// digit run that the following whitespace cannot extend, so a greedy scan
// finds exactly what the backtracking regex would.
size_t dotted_outline(std::string_view s) {  // \d+(?:\.\d+)+\.?
  size_t at = digits(s, 0);
  if (at == kNoMatch) return kNoMatch;
  int groups = 0;
  while (is_char(s, at, '.') && digit_length(s, at + 1) > 0) {
    at = digits(s, at + 1);
    ++groups;
  }
  if (groups == 0) return kNoMatch;
  return is_char(s, at, '.') ? at + 1 : at;
}

size_t number_letter(std::string_view s, size_t at, char close) {  // \d+\.?[a-zA-Z]<close>
  at = digits(s, at);
  if (at == kNoMatch) return kNoMatch;
  if (is_char(s, at, '.')) ++at;
  if (!is_letter(s, at)) return kNoMatch;
  return then_char(s, at + 1, close);
}

size_t numbered_marker_end(std::string_view s) {
  using Pattern = size_t (*)(std::string_view);
  static constexpr std::array<Pattern, 14> kPatterns = {
      dotted_outline,
      [](std::string_view t) { return number_letter(t, 0, '.'); },
      [](std::string_view t) { return number_letter(t, 0, ')'); },
      [](std::string_view t) {  // \(\d+\.?[a-zA-Z]\)
        return is_char(t, 0, '(') ? number_letter(t, 1, ')') : kNoMatch;
      },
      [](std::string_view t) { return then_char(t, digits(t, 0), '.'); },
      [](std::string_view t) { return then_char(t, digits(t, 0), ')'); },
      [](std::string_view t) {  // \(\d+\)
        return is_char(t, 0, '(') ? then_char(t, digits(t, 1), ')') : kNoMatch;
      },
      [](std::string_view t) {  // \[\d+\]
        return is_char(t, 0, '[') ? then_char(t, digits(t, 1), ']') : kNoMatch;
      },
      [](std::string_view t) { return then_char(t, run_of(t, 0, "ivxlcdm"), '.'); },
      [](std::string_view t) { return then_char(t, run_of(t, 0, "IVXLCDM"), '.'); },
      [](std::string_view t) { return is_lower(t, 0) ? then_char(t, 1, '.') : kNoMatch; },
      [](std::string_view t) { return is_upper(t, 0) ? then_char(t, 1, '.') : kNoMatch; },
      [](std::string_view t) { return is_lower(t, 0) ? then_char(t, 1, ')') : kNoMatch; },
      [](std::string_view t) { return is_upper(t, 0) ? then_char(t, 1, ')') : kNoMatch; },
  };
  for (const Pattern pattern : kPatterns) {
    const size_t end = pattern(s);
    if (end == kNoMatch || end >= s.size()) continue;
    // `^(marker)\s(.+)`: one whitespace code point, then content.
    const CodePoint separator = decode_at(s, end);
    if (is_space(separator.value) && end + separator.length < s.size()) return end;
  }
  return kNoMatch;
}

}  // namespace

std::optional<ListMarkerSplit> split_list_marker(std::string_view orig) {
  if (orig.empty()) return std::nullopt;
  size_t marker_end = kNoMatch;
  bool enumerated = false;
  const CodePoint first = decode_at(orig, 0);
  if (is_bullet(first.value) && first.length < orig.size()) {
    const CodePoint separator = decode_at(orig, first.length);
    if (is_space(separator.value) && first.length + separator.length < orig.size()) {
      marker_end = first.length;
    }
  }
  if (marker_end == kNoMatch) {
    marker_end = numbered_marker_end(orig);
    enumerated = marker_end != kNoMatch;
  }
  if (marker_end == kNoMatch) return std::nullopt;

  const CodePoint separator = decode_at(orig, marker_end);
  ListMarkerSplit split;
  split.marker = std::string(orig.substr(0, marker_end));
  split.text = std::string(orig.substr(marker_end + separator.length));
  split.enumerated = enumerated;
  size_t marker_code_points = 0;
  for (size_t at = 0; at < marker_end; ++marker_code_points) {
    at += decode_at(orig, at).length;
  }
  split.prefix_code_points = marker_code_points + 1;
  return split;
}

}  // namespace grparse
