// The JSON-LD decoder: a block becomes typed items, nothing a JSON-LD
// document states is dropped to make it fit, and a block that is not a
// JSON-LD document is refused with a reason and leaves no items behind.

#include <string>
#include <string_view>

#include <google/protobuf/text_format.h>
#include <google/protobuf/util/message_differencer.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/structured_data.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

using grparse::JsonLdDecode;
using grparse::decode_json_ld;
using grparse_test::require;

namespace {

// Decodes `json` and requires the result to equal `expected`, a
// StructuredData in protobuf text form without the dialect, which every
// decode sets.
void require_decodes(std::string_view json, std::string_view expected, std::string_view what,
                     uint64_t inline_contexts = 0) {
  docv1::StructuredData actual;
  const JsonLdDecode decode = decode_json_ld(json, &actual);
  require(decode.decoded, std::string(what) + ": decodes: " + decode.error);
  require(decode.inline_contexts == inline_contexts,
          std::string(what) + ": inline contexts counted: " +
              std::to_string(decode.inline_contexts));
  docv1::StructuredData wanted;
  require(google::protobuf::TextFormat::ParseFromString(std::string(expected), &wanted),
          std::string(what) + ": the expectation is well formed");
  wanted.set_dialect(docv1::STRUCTURED_DATA_KIND_JSON_LD);
  require(google::protobuf::util::MessageDifferencer::Equals(actual, wanted),
          std::string(what) + ": got " + actual.ShortDebugString());
}

void verify_a_node_is_an_item() {
  require_decodes(
      R"({"@context": "https://schema.org", "@id": "urn:a", "@type": ["Article", "Thing"],
          "headline": "H", "wordCount": 12, "ratingValue": 4.5, "free": false})",
      R"(contexts: "https://schema.org"
         items { id: "urn:a" types: "Article" types: "Thing"
                 properties { name: "headline" values { text: "H" } }
                 properties { name: "wordCount" values { integer: 12 } }
                 properties { name: "ratingValue" values { number: 4.5 } }
                 properties { name: "free" values { boolean: false } } })",
      "a node with every scalar kind");
}

void verify_values_keep_their_shape() {
  require_decodes(
      R"({"author": {"@type": "Person", "name": "Ada"},
          "sameAs": {"@id": "urn:b"},
          "tags": ["x", 2, {"@id": "urn:c"}, ["nested", "array"]],
          "label": {"@value": "Analyse", "@language": "de"},
          "date": {"@value": "2026-11-24", "@type": "Date"},
          "count": {"@value": 3},
          "steps": {"@list": ["one", "two"]},
          "kinds": {"@set": ["a"]},
          "nothing": null,
          "empty": []})",
      R"(items {
           properties { name: "author"
                        values { item { types: "Person"
                                        properties { name: "name" values { text: "Ada" } } } } }
           properties { name: "sameAs" values { reference: "urn:b" } }
           properties { name: "tags" values { text: "x" } values { integer: 2 }
                        values { reference: "urn:c" } values { text: "nested" }
                        values { text: "array" } }
           properties { name: "label" values { text: "Analyse" language: "de" } }
           properties { name: "date" values { text: "2026-11-24" datatype: "Date" } }
           properties { name: "count" values { integer: 3 } }
           properties { name: "steps" values { text: "one" } values { text: "two" } }
           properties { name: "kinds" values { text: "a" } }
           properties { name: "nothing" }
           properties { name: "empty" } })",
      "nested nodes, references, arrays, value objects, lists and nulls");
}

void verify_graphs_and_arrays_are_their_nodes() {
  require_decodes(
      R"({"@context": ["https://schema.org", "https://example.org/ctx"],
          "@graph": [{"@type": "A"}, {"@type": "B"}]})",
      R"(contexts: "https://schema.org" contexts: "https://example.org/ctx"
         items { types: "A" } items { types: "B" })",
      "a top-level graph is its nodes");
  require_decodes(
      R"([{"@context": "https://schema.org", "@type": "A"},
          {"@context": "https://schema.org", "@type": "B"}])",
      R"(contexts: "https://schema.org" items { types: "A" } items { types: "B" })",
      "a top-level array is its nodes, and a context named twice is listed once");
  // A graph that has a name of its own is a node with a @graph property:
  // dropping the @id to flatten it would lose which graph the nodes are in.
  require_decodes(
      R"({"@id": "urn:g", "@graph": [{"@type": "A"}]})",
      R"(items { id: "urn:g"
                 properties { name: "@graph" values { item { types: "A" } } } })",
      "a named graph keeps its name");
}

// Shapes JSON-LD does not define are kept as written, never trimmed to fit.
void verify_odd_shapes_lose_nothing() {
  require_decodes(
      R"({"@id": 7, "@type": {"x": 1}, "@id": "urn:a", "@id": "urn:again",
          "v": {"@value": "t", "@index": "i"},
          "w": {"@value": {"deep": true}},
          "x": {"@id": "urn:b", "name": "N"},
          "dup": 1, "dup": 2})",
      R"(items { id: "urn:a"
           properties { name: "@id" values { integer: 7 } }
           properties { name: "@type" values { item { properties { name: "x" values { integer: 1 } } } } }
           properties { name: "@id" values { text: "urn:again" } }
           properties { name: "v" values { item {
               properties { name: "@value" values { text: "t" } }
               properties { name: "@index" values { text: "i" } } } } }
           properties { name: "w" values { item {
               properties { name: "@value" values { item {
                   properties { name: "deep" values { boolean: true } } } } } } } }
           properties { name: "x" values { item { id: "urn:b"
               properties { name: "name" values { text: "N" } } } } }
           properties { name: "dup" values { integer: 1 } }
           properties { name: "dup" values { integer: 2 } } })",
      "ill-typed keywords, extra value-object members and repeated names");
}

void verify_inline_contexts_are_counted() {
  require_decodes(
      R"({"@context": ["https://schema.org", {"t": "http://purl.org/dc/terms/title"}, null],
          "t": "T", "part": {"@context": {"x": "urn:x"}, "x": 1}})",
      R"(contexts: "https://schema.org"
         items { properties { name: "t" values { text: "T" } }
                 properties { name: "part" values { item {
                     properties { name: "x" values { integer: 1 } } } } } })",
      "inline contexts are counted, not listed and not properties", 2);
}

void require_refused(std::string_view json, std::string_view reason, std::string_view what) {
  docv1::StructuredData data;
  const JsonLdDecode decode = decode_json_ld(json, &data);
  require(!decode.decoded && decode.error.contains(reason),
          std::string(what) + ": refused with the reason: " + decode.error);
  require(data.items_size() == 0 && data.contexts_size() == 0 &&
              data.dialect() == docv1::STRUCTURED_DATA_KIND_JSON_LD,
          std::string(what) + ": the entry names its dialect and holds nothing else: " +
              data.ShortDebugString());
}

void verify_what_is_not_json_ld_is_refused() {
  require_refused(R"({"@type": "Article",})", "at byte 20", "a trailing comma");
  require_refused("", "at byte 0", "an empty block");
  require_refused("\"just a string\"", "not a JSON-LD document", "a string at the top");
  require_refused("[1, 2]", "not a JSON-LD document", "an array of numbers");
  require_refused(R"([{"@type": "A"}, "b"])", "not a JSON-LD document",
                  "an array that is not all objects leaves no half result");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("structured-data-test", "ok", {
      verify_a_node_is_an_item,
      verify_values_keep_their_shape,
      verify_graphs_and_arrays_are_their_nodes,
      verify_odd_shapes_lose_nothing,
      verify_inline_contexts_are_counted,
      verify_what_is_not_json_ld_is_refused,
  });
}
