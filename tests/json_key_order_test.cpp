// The JSON key-order pass: objects printed from map fields get their members
// sorted (numerically for integer keys, bytewise otherwise); every other byte
// of a compact printer's output is copied through unchanged.
#include <set>
#include <stdexcept>
#include <string>

#include "../src/render/json_key_order.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "support/check.h"

using grparse::render::map_field_names;
using grparse::render::sort_map_objects;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

const std::set<std::string> kMaps{"pages", "custom_fields"};

void verify_integer_keys_sort_numerically() {
  require_equal(sort_map_objects(R"({"pages":{"10":{"n":10},"2":{"n":2},"1":{"n":1}}})", kMaps),
                std::string(R"({"pages":{"1":{"n":1},"2":{"n":2},"10":{"n":10}}})"),
                "page numbers sort as numbers, not as text");
}

void verify_string_keys_sort_bytewise() {
  require_equal(sort_map_objects(R"({"custom_fields":{"zeta":1,"alpha":"a,b}","mu":[1,{"z":1,"a":2}]}})", kMaps),
                std::string(R"({"custom_fields":{"alpha":"a,b}","mu":[1,{"z":1,"a":2}],"zeta":1}})"),
                "string keys sort bytewise; values, strings with braces and arrays pass through");
}

void verify_ordinary_objects_keep_their_order() {
  const std::string text = R"({"z":1,"a":{"y":[],"b":{}},"m":"x\"y\\"})";
  require_equal(sort_map_objects(text, kMaps), text, "message objects keep field order and escapes");
}

void verify_nesting_and_whitespace() {
  require_equal(sort_map_objects(R"({"texts":[{"meta":{"custom_fields":{"b":true,"a":null}}}]})", kMaps),
                std::string(R"({"texts":[{"meta":{"custom_fields":{"a":null,"b":true}}}]})"),
                "a map field nested in an array of messages is found");
  require_equal(sort_map_objects(" { \"pages\" : { \"2\" : 2 , \"1\" : 1 } } ", kMaps),
                std::string(R"({"pages":{"1":1,"2":2}})"),
                "whitespace between tokens is dropped, which a compact printer never emits anyway");
  require_equal(sort_map_objects(R"({"pages":{}})", kMaps), std::string(R"({"pages":{}})"), "an empty map");
}

void verify_bad_text_is_refused() {
  bool refused = false;
  try {
    sort_map_objects(R"({"pages":{"1":1)", kMaps);
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  require(refused, "an unterminated object is refused, not silently emitted");
}

void verify_document_map_fields_are_found_from_the_descriptor() {
  const std::set<std::string> names = map_field_names(ai::pipestream::document::v1::Document::descriptor());
  require(names.contains("pages"), "the page map is a map field");
  require(names.contains("custom_fields"), "meta custom fields are map fields");
  require(!names.contains("texts"), "a repeated message field is not a map");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("json-key-order-test", "ok", {
      verify_integer_keys_sort_numerically,
      verify_string_keys_sort_bytewise,
      verify_ordinary_objects_keep_their_order,
      verify_nesting_and_whitespace,
      verify_bad_text_is_refused,
      verify_document_map_fields_are_found_from_the_descriptor,
  });
}
