// The strict JSON reader: it reads exactly RFC 8259, keeps member order,
// repeated names and the integer or fraction spelling of a number, and
// refuses everything else whole, with a reason and an offset.

#include <string>
#include <string_view>

#include "grparse/json_reader.h"
#include "support/check.h"

using grparse::json::Limits;
using grparse::json::Parsed;
using grparse::json::Value;
using grparse::json::parse;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

void verify_scalars_keep_their_kind() {
  require(parse("null").ok && parse("null").value.kind == Value::Kind::kNull, "null reads");
  const Parsed yes = parse(" true ");
  require(yes.ok && yes.value.kind == Value::Kind::kBoolean && yes.value.boolean, "true reads");
  const Parsed no = parse("false");
  require(no.ok && no.value.kind == Value::Kind::kBoolean && !no.value.boolean, "false reads");
  const Parsed text = parse("\"a\"");
  require(text.ok && text.value.kind == Value::Kind::kString, "a string reads");
  require_equal(text.value.string, "a", "the string's content");
}

// A JSON-LD consumer tells 1200 words from a rating of 4.5: the reader
// keeps the spelling the source chose.
void verify_integers_and_fractions_stay_apart() {
  const Parsed whole = parse("-1200");
  require(whole.ok && whole.value.kind == Value::Kind::kInteger && whole.value.integer == -1200,
          "a number with no fraction and no exponent is an integer");
  const Parsed largest = parse("9223372036854775807");
  require(largest.ok && largest.value.kind == Value::Kind::kInteger &&
              largest.value.integer == INT64_MAX,
          "the largest 64-bit integer is exact");
  const Parsed too_large = parse("9223372036854775808");
  require(too_large.ok && too_large.value.kind == Value::Kind::kNumber &&
              too_large.value.number == 9223372036854775808.0,
          "an integer past 64 bits is the nearest double, not an error and not a wrapped value");
  const Parsed fraction = parse("4.5");
  require(fraction.ok && fraction.value.kind == Value::Kind::kNumber &&
              fraction.value.number == 4.5,
          "a fraction is a number");
  const Parsed exponent = parse("1e3");
  require(exponent.ok && exponent.value.kind == Value::Kind::kNumber &&
              exponent.value.number == 1000.0,
          "an exponent makes a number, even when its value is whole");
  const Parsed zero = parse("-0");
  require(zero.ok && zero.value.kind == Value::Kind::kInteger && zero.value.integer == 0,
          "minus zero reads");
}

void verify_objects_keep_order_and_repeats() {
  const Parsed parsed = parse(R"({"b": 1, "a": [true, null], "b": {"c": "d"}})");
  require(parsed.ok, "an object reads: " + parsed.error);
  const Value& object = parsed.value;
  require(object.kind == Value::Kind::kObject && object.members.size() == 3,
          "three members, the repeated name included");
  require_equal(object.members[0].name, "b", "first member");
  require_equal(object.members[1].name, "a", "second member, in source order");
  require_equal(object.members[2].name, "b", "the repeated name stays");
  require(object.members[1].value.kind == Value::Kind::kArray &&
              object.members[1].value.elements.size() == 2 &&
              object.members[1].value.elements[1].kind == Value::Kind::kNull,
          "an array keeps its elements in order");
  require(object.find("b") == &object.members[0].value, "find returns the first of a name");
  require(object.find("missing") == nullptr, "find returns nothing for an absent name");
  require(parse("{}").ok && parse("[]").ok && parse(" [ ] ").ok, "empty containers read");
}

void verify_string_escapes_resolve_to_utf8() {
  const Parsed parsed = parse(R"("q\" s\\ f\/ \b\f\n\r\t \u00e9 \ud83d\ude00 \u0041")");
  require(parsed.ok, "every JSON escape reads: " + parsed.error);
  require_equal(parsed.value.string,
                std::string("q\" s\\ f/ \b\f\n\r\t \xC3\xA9 \xF0\x9F\x98\x80 A"),
                "escapes resolve, a surrogate pair to one character");
  const Parsed raw = parse("\"caf\xC3\xA9\"");
  require(raw.ok && raw.value.string == "caf\xC3\xA9", "UTF-8 in a string passes through");
}

void require_refused(std::string_view text, std::string_view why) {
  const Parsed parsed = parse(text);
  require(!parsed.ok && !parsed.error.empty() &&
              parsed.value.kind == Value::Kind::kNull && parsed.value.members.empty() &&
              parsed.value.elements.empty(),
          std::string(why) + " is refused whole, with a reason: '" + std::string(text) + "'");
}

// What is not JSON by the RFC is refused, never repaired: the output is
// presented as what the source says.
void verify_everything_else_is_refused() {
  require_refused("", "empty text");
  require_refused("   ", "whitespace alone");
  require_refused("{\"a\": 1,}", "a trailing comma in an object");
  require_refused("[1, 2,]", "a trailing comma in an array");
  require_refused("{'a': 1}", "single quotes");
  require_refused("{a: 1}", "an unquoted name");
  require_refused("{\"a\" 1}", "a missing colon");
  require_refused("[1 2]", "a missing comma");
  require_refused("{\"a\": 1} x", "text after the value");
  require_refused("{\"a\": 1}{\"b\": 2}", "two values");
  require_refused("[1", "an unclosed array");
  require_refused("{\"a\": ", "an unclosed object");
  require_refused("\"abc", "an unclosed string");
  require_refused("\"a\nb\"", "a raw line break in a string");
  require_refused("\"\\x41\"", "an escape JSON does not have");
  require_refused("\"\\u12\"", "a short \\u escape");
  require_refused("\"\\ud83d\"", "a high surrogate alone");
  require_refused("\"\\ud83dx\"", "a high surrogate followed by a character");
  require_refused("\"\\ude00\"", "a low surrogate alone");
  require_refused("01", "a leading zero");
  require_refused("1.", "a decimal point with no digit");
  require_refused("1e", "an exponent with no digit");
  require_refused("+1", "a plus sign");
  require_refused(".5", "a number that starts with a point");
  require_refused("NaN", "NaN");
  require_refused("1e999", "a number past the range of a double");
  require_refused("tru", "a cut literal");
  require_refused("<!-- {} -->", "an HTML comment around the value");
  require_refused("\xEF\xBB\xBF{}", "a byte order mark");
  require_refused("\"\xC3\"", "text that is not UTF-8");
  const Parsed where = parse("[1, 2, x]");
  require(!where.ok && where.offset == 7, "the offset names the byte that is wrong, got " +
                                              std::to_string(where.offset));
}

// A hostile block costs a bounded stack and a bounded heap.
void verify_depth_and_size_are_bounded() {
  const std::string at_limit = std::string(64, '[') + std::string(64, ']');
  require(parse(at_limit).ok, "64 levels read");
  const std::string too_deep = std::string(65, '[') + std::string(65, ']');
  const Parsed deep = parse(too_deep);
  require(!deep.ok && deep.error.contains("deeper"), "65 levels are refused: " + deep.error);
  require(!parse(std::string(100000, '[')).ok, "a hundred thousand open brackets are refused");

  Limits small;
  small.max_values = 4;
  require(parse("[1, 2, 3]", small).ok, "four values fit a limit of four");
  const Parsed many = parse("[1, 2, 3, 4]", small);
  require(!many.ok && many.error.contains("values"), "five values do not: " + many.error);
}

}  // namespace

int main() {
  return grparse_test::run_test_main("json-reader-test", "ok", {
      verify_scalars_keep_their_kind,
      verify_integers_and_fractions_stay_apart,
      verify_objects_keep_order_and_repeats,
      verify_string_escapes_resolve_to_utf8,
      verify_everything_else_is_refused,
      verify_depth_and_size_are_bounded,
  });
}
