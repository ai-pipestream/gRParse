#include "token_counter.h"

#include <cstddef>
#include <string>

namespace grparse::chunking {
namespace {

constexpr char32_t kReplacement = 0xFFFD;

// Length of the UTF-8 sequence a lead byte announces; 0 for a byte that
// cannot lead one.
int sequence_length(unsigned char lead) {
  if (lead < 0x80U) return 1;
  if ((lead & 0xE0U) == 0xC0U) return 2;
  if ((lead & 0xF0U) == 0xE0U) return 3;
  if ((lead & 0xF8U) == 0xF0U) return 4;
  return 0;
}

bool is_continuation(unsigned char byte) { return (byte & 0xC0U) == 0x80U; }

// True for the letters of the Latin-1 supplement, the Greek and Coptic
// block's letters, and the Cyrillic blocks. These are the alphabetic ranges
// rule 3 joins into words beyond ASCII.
bool is_extended_letter(char32_t code_point) {
  // Latin-1 supplement: the ordinal indicators, micro sign, and the two
  // accented letter runs (the multiplication and division signs sit inside
  // the range and are symbols, not letters).
  if (code_point == 0x00AA || code_point == 0x00B5 || code_point == 0x00BA) return true;
  if (code_point >= 0x00C0 && code_point <= 0x00FF) {
    return code_point != 0x00D7 && code_point != 0x00F7;
  }
  // Latin Extended-A and Extended-B: the accented and digraph letters the
  // European languages spell with.
  if (code_point >= 0x0100 && code_point <= 0x024F) return true;
  // Greek and Coptic: the letter runs, leaving the reserved slots out.
  if (code_point == 0x0386) return true;
  if (code_point >= 0x0388 && code_point <= 0x03FF) {
    return code_point != 0x038B && code_point != 0x038D && code_point != 0x03A2;
  }
  // Cyrillic and Cyrillic supplement.
  if (code_point >= 0x0400 && code_point <= 0x052F) return true;
  return false;
}

// True for the code points rule 4 counts one apiece: CJK unified ideographs
// (the base block and extension A), the compatibility ideographs, and the
// kana blocks.
bool is_standalone_token(char32_t code_point) {
  if (code_point >= 0x3041 && code_point <= 0x30FF) return true;   // kana
  if (code_point >= 0x3400 && code_point <= 0x4DBF) return true;   // CJK ext A
  if (code_point >= 0x4E00 && code_point <= 0x9FFF) return true;   // CJK unified
  if (code_point >= 0xF900 && code_point <= 0xFAFF) return true;   // compatibility
  return false;
}

bool is_alphanumeric(char32_t code_point) {
  if (code_point < 0x80) {
    return (code_point >= U'0' && code_point <= U'9') ||
           (code_point >= U'A' && code_point <= U'Z') ||
           (code_point >= U'a' && code_point <= U'z');
  }
  return is_extended_letter(code_point);
}

}  // namespace

bool is_wordish_whitespace(char32_t code_point) {
  if (code_point <= 0x20) return true;  // ASCII controls and space
  if (code_point == 0x7F) return true;  // DEL
  switch (code_point) {
    case 0x0085:  // NEL
    case 0x00A0:  // NBSP
    case 0x1680:  // Ogham space mark
    case 0x2028:  // line separator
    case 0x2029:  // paragraph separator
    case 0x202F:  // narrow no-break space
    case 0x205F:  // medium mathematical space
    case 0x3000:  // ideographic space
      return true;
    default:
      break;
  }
  return code_point >= 0x2000 && code_point <= 0x200A;
}

std::vector<char32_t> decode_utf8(std::string_view text) {
  std::vector<char32_t> points;
  points.reserve(text.size());
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    const int length = sequence_length(lead);
    if (length == 0 || index + static_cast<std::size_t>(length) > text.size()) {
      points.push_back(kReplacement);
      ++index;
      continue;
    }
    char32_t value = 0;
    switch (length) {
      case 1: value = lead; break;
      case 2: value = lead & 0x1FU; break;
      case 3: value = lead & 0x0FU; break;
      default: value = lead & 0x07U; break;
    }
    bool valid = true;
    for (int offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(text[index + static_cast<std::size_t>(offset)]);
      if (!is_continuation(next)) {
        valid = false;
        break;
      }
      value = (value << 6) | (next & 0x3FU);
    }
    // Overlong forms, surrogates, and out-of-range values are as malformed as
    // a bad continuation byte and degrade the same way.
    if (!valid || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF) ||
        (length == 2 && value < 0x80) || (length == 3 && value < 0x800) ||
        (length == 4 && value < 0x10000)) {
      points.push_back(kReplacement);
      ++index;
      continue;
    }
    points.push_back(value);
    index += static_cast<std::size_t>(length);
  }
  return points;
}

std::string encode_utf8(const char32_t* begin, const char32_t* end) {
  std::string out;
  out.reserve(static_cast<std::size_t>(end - begin));
  for (const char32_t* point = begin; point != end; ++point) {
    const char32_t value = *point;
    if (value < 0x80) {
      out.push_back(static_cast<char>(value));
    } else if (value < 0x800) {
      out.push_back(static_cast<char>(0xC0U | (value >> 6)));
      out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if (value < 0x10000) {
      out.push_back(static_cast<char>(0xE0U | (value >> 12)));
      out.push_back(static_cast<char>(0x80U | ((value >> 6) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else {
      out.push_back(static_cast<char>(0xF0U | (value >> 18)));
      out.push_back(static_cast<char>(0x80U | ((value >> 12) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | ((value >> 6) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    }
  }
  return out;
}

std::size_t codepoint_length(std::string_view text) {
  return decode_utf8(text).size();
}

int count_tokens(const char32_t* begin, const char32_t* end) {
  int tokens = 0;
  bool inside_word = false;
  for (const char32_t* point = begin; point != end; ++point) {
    const char32_t value = *point;
    if (is_wordish_whitespace(value)) {
      inside_word = false;
      continue;
    }
    if (is_standalone_token(value)) {
      ++tokens;
      inside_word = false;
      continue;
    }
    if (is_alphanumeric(value)) {
      if (!inside_word) {
        ++tokens;
        inside_word = true;
      }
      continue;
    }
    ++tokens;
    inside_word = false;
  }
  return tokens;
}

int count_tokens(std::string_view text) {
  const std::vector<char32_t> points = decode_utf8(text);
  return count_tokens(points.data(), points.data() + points.size());
}

}  // namespace grparse::chunking
