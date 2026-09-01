#include "storage_text.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace grparse::confluence {

std::string lowercase(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char letter) {
    return static_cast<char>(std::tolower(letter));
  });
  return value;
}

std::string uppercase(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char letter) {
    return static_cast<char>(std::toupper(letter));
  });
  return value;
}

bool ends_with(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_space(char letter) {
  return letter == ' ' || letter == '\t' || letter == '\n' || letter == '\r' ||
         letter == '\f' || letter == '\v';
}

bool blank(std::string_view text) {
  return std::ranges::all_of(text, is_space);
}

std::string_view trim(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() && is_space(text[begin])) ++begin;
  size_t end = text.size();
  while (end > begin && is_space(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

long long code_points(std::string_view text) {
  long long count = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0) != 0x80) ++count;
  }
  return count;
}

void append_code_point(std::uint32_t code_point, std::string* out) {
  if (code_point < 0x80) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

namespace {

// The five XML predefined references plus the non-breaking space. Appends the
// replacement and reports whether the name was one of them.
bool append_named_entity(std::string_view name, std::string* out) {
  if (name == "amp") {
    out->push_back('&');
  } else if (name == "lt") {
    out->push_back('<');
  } else if (name == "gt") {
    out->push_back('>');
  } else if (name == "quot") {
    out->push_back('"');
  } else if (name == "apos") {
    out->push_back('\'');
  } else if (name == "nbsp") {
    append_code_point(0x00A0, out);
  } else {
    return false;
  }
  return true;
}

// The value of a "#1234" or "#x4d2" reference, or nothing when the digits are
// absent, malformed, out of range, or name a code point with no encoding.
bool numeric_entity_value(std::string_view name, std::uint32_t* code_point) {
  const bool hex = name[1] == 'x' || name[1] == 'X';
  const std::string_view digits = name.substr(hex ? 2 : 1);
  if (digits.empty()) return false;
  std::uint32_t value = 0;
  for (const char digit : digits) {
    const int place = std::isdigit(static_cast<unsigned char>(digit)) != 0
                          ? digit - '0'
                      : (hex && std::isxdigit(static_cast<unsigned char>(digit)) != 0)
                          ? std::tolower(static_cast<unsigned char>(digit)) - 'a' + 10
                          : -1;
    if (place < 0) return false;
    value = value * (hex ? 16 : 10) + static_cast<std::uint32_t>(place);
    if (value > 0x10FFFF) return false;
  }
  if (value == 0 || (value >= 0xD800 && value <= 0xDFFF)) return false;
  *code_point = value;
  return true;
}

// Appends the replacement for the reference named between "&" and ";", or
// reports that it has none.
bool append_entity(std::string_view name, std::string* out) {
  if (append_named_entity(name, out)) return true;
  if (name.size() <= 1 || name[0] != '#') return false;
  std::uint32_t code_point = 0;
  if (!numeric_entity_value(name, &code_point)) return false;
  append_code_point(code_point, out);
  return true;
}

}  // namespace

std::string decode_entities(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  size_t index = 0;
  while (index < raw.size()) {
    if (raw[index] != '&') {
      out.push_back(raw[index++]);
      continue;
    }
    const size_t semicolon = raw.find(';', index + 1);
    if (semicolon == std::string_view::npos || semicolon - index > 16) {
      out.push_back(raw[index++]);
      continue;
    }
    if (!append_entity(raw.substr(index + 1, semicolon - index - 1), &out)) {
      out.push_back(raw[index++]);
      continue;
    }
    index = semicolon + 1;
  }
  return out;
}

}  // namespace grparse::confluence
