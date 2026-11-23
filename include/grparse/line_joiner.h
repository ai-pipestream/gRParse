#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace grparse {

// Joins line texts into one string. pdfium marks a hyphen it took for a
// line-break hyphen with U+FFFE: the mark and any whitespace after it go, so
// "Sci<FFFE>" + "entific" reads "Scientific". A mark before an uppercase
// letter or a digit stays a hyphen ("COVID-19"), since those halves are
// words of their own.
class LineJoiner {
 public:
  explicit LineJoiner(char separator = ' ') : separator_(separator) {}
  // Appends one line and returns the code point offset where it starts.
  uint64_t add(const std::string& line);
  const std::string& text() const { return text_; }
  std::string take() { return std::move(text_); }
  uint64_t code_points() const { return code_points_; }

 private:
  char separator_;
  std::string text_;
  uint64_t code_points_ = 0;
  bool open_break_ = false;
};

}  // namespace grparse
