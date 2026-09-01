#include "markdown_text.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace grparse::render {

std::string stripped(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\n\r\f\v");
  if (begin == std::string_view::npos) return std::string();
  const auto end = text.find_last_not_of(" \t\n\r\f\v");
  return std::string(text.substr(begin, end - begin + 1));
}

std::string escape_underscores(const std::string& text) {
  const auto escape_span = [&text](std::size_t from, std::size_t to,
                                   std::string* out) {
    for (std::size_t i = from; i < to; ++i) {
      if (text[i] == '_' && (i == 0 || text[i - 1] != '\\')) {
        out->append("\\_");
      } else {
        out->push_back(text[i]);
      }
    }
  };
  // The end of an image match starting at `start`, or npos when the pattern
  // does not complete. Mirrors the lazy quantifiers: the first "](" that
  // closes the alt text, then the first ")" that closes the target.
  const auto image_match_end = [&text](std::size_t start) -> std::size_t {
    for (std::size_t close = start + 2; close < text.size(); ++close) {
      if (text[close] == '\n') return std::string::npos;
      if (text[close] != ']') continue;
      if (close + 1 >= text.size() || text[close + 1] != '(') continue;
      for (std::size_t end = close + 2; end < text.size(); ++end) {
        if (text[end] == '\n') return std::string::npos;
        if (text[end] == ')') return end + 1;
      }
      return std::string::npos;
    }
    return std::string::npos;
  };

  std::string out;
  out.reserve(text.size());
  std::size_t last_end = 0;
  std::size_t at = 0;
  while (at + 1 < text.size()) {
    if (text[at] != '!' || text[at + 1] != '[') {
      ++at;
      continue;
    }
    const std::size_t match_end = image_match_end(at);
    if (match_end == std::string::npos) {
      ++at;
      continue;
    }
    escape_span(last_end, at, &out);
    out.append(text, at, match_end - at);
    last_end = match_end;
    at = match_end;
  }
  escape_span(last_end, text.size(), &out);
  return out;
}

std::string md_line_breaks(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t split = text.find("\n\n", at);
    const std::size_t end = split == std::string::npos ? text.size() : split;
    for (std::size_t i = at; i < end; ++i) {
      if (text[i] == '\n') out.append("  ");
      out.push_back(text[i]);
    }
    if (split == std::string::npos) break;
    out.append("\n\n");
    at = split + 2;
  }
  return out;
}

std::string heading_line_breaks(std::string text) {
  std::ranges::replace(text, '\n', ' ');
  return text;
}

std::string humanized(std::string text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '_' && i + 1 < text.size() && text[i + 1] == '_') ++i;
    out.push_back(text[i] == '_' ? ' ' : text[i]);
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    const auto c = static_cast<unsigned char>(out[i]);
    out[i] = static_cast<char>(i == 0 ? std::toupper(c) : std::tolower(c));
  }
  return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (const auto& part : parts) {
    if (part.empty()) continue;
    if (!out.empty()) out.append(sep);
    out.append(part);
  }
  return out;
}

}  // namespace grparse::render
