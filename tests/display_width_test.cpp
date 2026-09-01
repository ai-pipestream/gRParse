// Terminal display width, the measurement the Markdown table formatter pads
// its columns with: the ASCII fast path, the East Asian wide and fullwidth
// sets, zero-width marks, the control-character sentinel, and what malformed
// UTF-8 measures as.  The tables themselves are generated data; these cases
// pin the behaviour around them.  Every non-ASCII code point is written as an
// escape so the file stays readable in any editor.

#include <string>

#include "../src/render/display_width.h"
#include "support/check.h"

using grparse::render::display_width;
using grparse_test::require_equal;

namespace {

// One case of the tables below: the bytes, the cells they occupy, and why.
struct WidthCase {
  std::string text;
  int width;
  std::string what;
};

void verify_ascii_measures_as_its_length() {
  const WidthCase cases[] = {
      {"", 0, "an empty string occupies no cells"},
      {" ", 1, "a space is one cell"},
      {"hello", 5, "printable ASCII measures as its byte count"},
      {"| a | b |", 9, "the table formatter's own punctuation is one cell each"},
      {"~", 1, "the last printable ASCII code point is one cell"},
  };
  for (const WidthCase& one : cases) require_equal(display_width(one.text), one.width, one.what);
}

void verify_east_asian_text_is_two_cells_per_code_point() {
  const WidthCase cases[] = {
      {"\u65e5", 2, "a CJK ideograph is two cells"},
      {"\u65e5\u672c\u8a9e", 6, "three ideographs are six cells"},
      {"\uff21", 2, "a fullwidth Latin capital is two cells"},
      {"\uac00", 2, "a Hangul syllable is two cells"},
      {"\u1100", 2, "the first wide range starts at U+1100"},
      {"\U0001f600", 2, "an emoji in the wide set is two cells"},
      {"A\uff22", 3, "ASCII and fullwidth text add up"},
      {"\u3042\u3044", 4, "kana are wide too"},
  };
  for (const WidthCase& one : cases) require_equal(display_width(one.text), one.width, one.what);
}

void verify_zero_width_code_points_add_nothing() {
  const WidthCase cases[] = {
      {"e\u0301", 1, "a combining acute accent rides its base letter"},
      {"a\u0300\u0301", 1, "stacked combining marks still ride one cell"},
      {"a\u200bb", 2, "a zero-width space adds nothing"},
      {"a\u200db", 2, "a zero-width joiner adds nothing"},
      {"\ufeffab", 2, "a byte order mark adds nothing"},
      {"a\ufe0f", 1, "a variation selector adds nothing"},
      {std::string("a\0b", 3), 2, "a NUL is the one control byte that measures zero"},
  };
  for (const WidthCase& one : cases) require_equal(display_width(one.text), one.width, one.what);
}

void verify_a_control_character_poisons_the_whole_string() {
  const WidthCase cases[] = {
      {"a\tb", -1, "a tab is a control character, so the string has no width"},
      {"a\nb", -1, "a newline has no width either"},
      {"a\rb", -1, "a carriage return has no width either"},
      {"\x1b[0m", -1, "an escape sequence has no width"},
      {"a\x7f", -1, "DEL is a control character"},
      {"a\xc2\x80", -1, "the first C1 control has no width"},
      {"a\xc2\x9f", -1, "the last C1 control has no width"},
  };
  for (const WidthCase& one : cases) require_equal(display_width(one.text), one.width, one.what);
  require_equal(display_width("a\xc2\xa0"), 2,
                "the code point just past the C1 block is an ordinary cell");
}

void verify_malformed_utf8_still_measures_deterministically() {
  const WidthCase cases[] = {
      {"caf\xc3", 4, "a truncated lead byte measures as itself"},
      {"\xc3(", 2, "a lead byte without its continuation measures as itself"},
      {"\xe6\x97", -1, "a truncated three-byte sequence leaves a stray continuation byte"},
      {"a\xc0\xaf", 2, "an overlong sequence measures as the code point it encodes"},
  };
  for (const WidthCase& one : cases) require_equal(display_width(one.text), one.width, one.what);
}

void verify_measurement_is_stable_across_calls() {
  const std::string mixed = "id | \u65e5\u672c | e\u0301";
  require_equal(display_width(mixed), display_width(mixed),
                "the same text measures the same twice");
  require_equal(display_width(mixed), 13,
                "a mixed row measures its ASCII, wide, and combining parts together");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("display-width-test", "ok", {
      verify_ascii_measures_as_its_length,
      verify_east_asian_text_is_two_cells_per_code_point,
      verify_zero_width_code_points_add_nothing,
      verify_a_control_character_poisons_the_whole_string,
      verify_malformed_utf8_still_measures_deterministically,
      verify_measurement_is_stable_across_calls,
  });
}
