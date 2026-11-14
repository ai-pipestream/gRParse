#include "grparse/line_joiner.h"

#include <cctype>
#include <string_view>

#include "grparse/document_assembly.h"

namespace grparse {
namespace {

constexpr std::string_view kHyphenationMark = "\xEF\xBF\xBE";

bool keeps_hyphen_before(std::string_view rest) {
  return !rest.empty() &&
         (std::isupper(static_cast<unsigned char>(rest.front())) != 0 ||
          std::isdigit(static_cast<unsigned char>(rest.front())) != 0);
}

}  // namespace

uint64_t LineJoiner::add(const std::string& line) {
  std::string clean;
  clean.reserve(line.size());
  bool ends_open = false;
  size_t i = 0;
  while (i < line.size()) {
    if (line.compare(i, kHyphenationMark.size(), kHyphenationMark) != 0) {
      clean.push_back(line[i++]);
      continue;
    }
    i += kHyphenationMark.size();
    size_t next = i;
    while (next < line.size() && std::isspace(static_cast<unsigned char>(line[next])) != 0) ++next;
    if (next == line.size()) {
      ends_open = true;
    } else if (keeps_hyphen_before(std::string_view(line).substr(next))) {
      clean.push_back('-');
    }
    i = next;
  }
  if (open_break_) {
    if (keeps_hyphen_before(clean)) {
      text_.push_back('-');
      ++code_points_;
    }
  } else if (!text_.empty()) {
    text_.push_back(separator_);
    ++code_points_;
  }
  const uint64_t start = code_points_;
  code_points_ += utf8_codepoint_count(clean);
  text_ += clean;
  // A mark at the very end of the text has no next line to join, so the
  // break only stays open while there is text before it.
  open_break_ = ends_open && !text_.empty();
  return start;
}

}  // namespace grparse
