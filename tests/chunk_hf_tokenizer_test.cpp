// The hf/1 token counter against the tiny committed tokenizer.json fixture
// (tests/data/chunk_tokenizer_tiny.json): a 27-entry BPE vocab with merges
// for "hello", "world", "abc", and "世界", poisoned with fixed-length
// padding (16) and a truncation limit (8) that the loader must strip, the
// way docling.rs found all-MiniLM-L6-v2's file ships them. The real-model
// tier is chunk_hf_model_test.cpp, which skips without a deployment's
// models/chunk/tokenizer.json.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "../src/chunking/chunker.h"
#include "../src/chunking/token_counter.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

using grparse::chunking::chunk_hybrid;
using grparse::chunking::load_hf_tokenizer_json;
using grparse::chunking::resolve_hf_tokenizer_path;
using grparse::chunking::TokenCounter;
using grparse::chunking::validate_hybrid_options;

namespace {

using grparse_test::require;

void require_eq(int actual, int expected, const std::string& message) {
  if (actual != expected) {
    throw std::runtime_error(message + "\nexpected: " + std::to_string(expected) +
                             "\nactual:   " + std::to_string(actual));
  }
}

std::string fixture_path() {
  const char* data = std::getenv("GRPARSE_TEST_DATA_DIR");
  require(data != nullptr, "GRPARSE_TEST_DATA_DIR must point at tests/data");
  return std::string(data) + "/chunk_tokenizer_tiny.json";
}

TokenCounter load_fixture() {
  TokenCounter counter;
  const grpc::Status status = TokenCounter::huggingface(fixture_path(), &counter);
  require(status.ok(), "the fixture tokenizer must load: " + status.error_message());
  return counter;
}

// Sets or clears an environment variable for the scope of a case and puts
// back whatever was there.
class ScopedEnv {
 public:
  ScopedEnv(std::string name, const char* value) : name_(std::move(name)) {
    if (const char* old = std::getenv(name_.c_str())) old_ = old;
    if (value == nullptr) {
      unsetenv(name_.c_str());
    } else {
      setenv(name_.c_str(), value, 1);
    }
  }
  ~ScopedEnv() {
    if (old_.has_value()) {
      setenv(name_.c_str(), old_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

 private:
  std::string name_;
  std::optional<std::string> old_;
};

std::string write_temp(const std::string& name, const std::string& content) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
  std::ofstream out(path, std::ios::binary);
  out << content;
  out.close();
  return path.string();
}

// -- resolution and loading ---------------------------------------------------

void verify_resolution_order() {
  require(resolve_hf_tokenizer_path("explicit.json") == "explicit.json",
          "the request's tokenizer_path wins");
  {
    ScopedEnv env("GRPARSE_CHUNK_TOKENIZER", "/env/tokenizer.json");
    require(resolve_hf_tokenizer_path("") == "/env/tokenizer.json",
            "GRPARSE_CHUNK_TOKENIZER is next");
  }
  {
    ScopedEnv chunk("GRPARSE_CHUNK_TOKENIZER", nullptr);
    ScopedEnv models("GRPARSE_MODELS_DIR", "/srv/models");
    require(resolve_hf_tokenizer_path("") == "/srv/models/chunk/tokenizer.json",
            "the models dir is the fallback");
  }
  {
    ScopedEnv chunk("GRPARSE_CHUNK_TOKENIZER", nullptr);
    ScopedEnv models("GRPARSE_MODELS_DIR", nullptr);
    require(resolve_hf_tokenizer_path("") == "/models/chunk/tokenizer.json",
            "an unset models dir defaults to /models");
  }
}

void verify_padding_and_truncation_are_stripped() {
  std::string json;
  require(load_hf_tokenizer_json(fixture_path(), &json).ok(), "the fixture must load");
  require(json.find("padding") == std::string::npos, "the padding member is stripped");
  require(json.find("truncation") == std::string::npos, "the truncation member is stripped");

  const TokenCounter counter = load_fixture();
  require_eq(counter.count("hello"), 1,
             "the fixture pads to 16 tokens; the stripped file counts the text");
  const std::string ten_hellos =
      "hello hello hello hello hello hello hello hello hello hello";
  require_eq(counter.count(ten_hellos), 10,
             "the fixture truncates at 8 tokens; the stripped file counts all ten");
}

void verify_hf_counts_match_the_fixture_vocab() {
  const TokenCounter counter = load_fixture();
  require(counter.rules() == "hf/1", "the counter reports its rule set");
  require(!counter.codepoint_bounded(), "a byte-level tokenizer breaks the code point bound");
  require_eq(counter.count(""), 0, "empty text counts zero");
  require_eq(counter.count("hello world"), 2, "both words merge whole");
  require_eq(counter.count("hello, world"), 3, "the comma is its own pretoken");
  require_eq(counter.count("abcabc"), 2, "BPE re-applies merges after a merge");
  require_eq(counter.count("世界"), 1, "the CJK merge joins the pretoken");
  require_eq(counter.count("世 界"), 2, "no merge spans a whitespace split");
  // 'H' is absent from the vocab and the model carries no unk token and no
  // byte fallback, so it drops out of the count entirely.
  require_eq(counter.count("Hello"), 4, "an unknown character drops: e l l o remain");
  require_eq(counter.count("helloworld"), 2, "one pretoken, two merged words");
}

void verify_load_failures_are_invalid_argument_not_crashes() {
  TokenCounter counter;
  const grpc::Status missing = TokenCounter::huggingface("/nonexistent/tokenizer.json", &counter);
  require(missing.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "a missing file is INVALID_ARGUMENT");
  require(missing.error_message().contains("/nonexistent/tokenizer.json"),
          "the error names the file: " + missing.error_message());

  const std::string garbage = write_temp("grparse-hf-garbage.json", "this is not json");
  const grpc::Status not_json = TokenCounter::huggingface(garbage, &counter);
  require(not_json.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "a non-JSON file is INVALID_ARGUMENT");

  const std::string trailing = write_temp("grparse-hf-trailing.json", "{} {}");
  require(TokenCounter::huggingface(trailing, &counter).error_code() ==
              grpc::StatusCode::INVALID_ARGUMENT,
          "trailing content after the object is INVALID_ARGUMENT");

  // Well-formed JSON that is not a tokenizer: the patched tokenizers-cpp
  // reports the parse failure as a null handle instead of aborting.
  const std::string not_a_tokenizer = write_temp("grparse-hf-notatok.json", "{\"foo\": 1}");
  const grpc::Status schema = TokenCounter::huggingface(not_a_tokenizer, &counter);
  require(schema.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "a JSON file that is not a tokenizer.json is INVALID_ARGUMENT");

  std::filesystem::remove(garbage);
  std::filesystem::remove(trailing);
  std::filesystem::remove(not_a_tokenizer);
}

// -- option validation ----------------------------------------------------------

parsev1::HybridChunkerOptions hf_options(int max_tokens, const std::string& path) {
  parsev1::HybridChunkerOptions options;
  options.set_max_tokens(max_tokens);
  options.set_tokenizer("hf/1");
  if (!path.empty()) options.set_tokenizer_path(path);
  return options;
}

void verify_validation_accepts_hf_and_names_the_resolution_order() {
  require(validate_hybrid_options(hf_options(64, fixture_path())).ok(),
          "hf/1 with a readable tokenizer.json is accepted");

  parsev1::HybridChunkerOptions missing = hf_options(64, "/nonexistent/tokenizer.json");
  const grpc::Status absent = validate_hybrid_options(missing);
  require(absent.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "hf/1 without a readable file is INVALID_ARGUMENT");
  require(absent.error_message().contains("tokenizer_path") &&
              absent.error_message().contains("GRPARSE_CHUNK_TOKENIZER") &&
              absent.error_message().contains("GRPARSE_MODELS_DIR"),
          "the rejection names the resolution order: " + absent.error_message());

  {
    // No request path, no env override, and a models dir without the file.
    ScopedEnv chunk("GRPARSE_CHUNK_TOKENIZER", nullptr);
    ScopedEnv models("GRPARSE_MODELS_DIR", "/nonexistent-models");
    const grpc::Status unresolved = validate_hybrid_options(hf_options(64, ""));
    require(unresolved.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
            "hf/1 with nothing to resolve to is INVALID_ARGUMENT");
    require(unresolved.error_message().contains("/nonexistent-models/chunk/tokenizer.json"),
            "the rejection names the path it tried: " + unresolved.error_message());
  }

  parsev1::HybridChunkerOptions misplaced_path;
  misplaced_path.set_max_tokens(64);
  misplaced_path.set_tokenizer_path(fixture_path());
  require(validate_hybrid_options(misplaced_path).error_code() ==
              grpc::StatusCode::INVALID_ARGUMENT,
          "tokenizer_path without hf/1 is rejected rather than ignored");

  parsev1::HybridChunkerOptions wordish;
  wordish.set_max_tokens(64);
  require(validate_hybrid_options(wordish).ok(), "the wordish default needs no file");
}

// -- chunking with hf/1 ---------------------------------------------------------

docv1::Document new_document() {
  docv1::Document document;
  document.set_schema_name("docling_document_v2");
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_furniture()->set_self_ref("#/furniture");
  return document;
}

void add_paragraph(docv1::Document* document, const std::string& text) {
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  auto* base = document->add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->set_text(text);
  base->set_orig(text);
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->mutable_parent()->set_ref("#/body");
  document->mutable_body()->add_children()->set_ref(ref);
}

std::vector<parsev1::Chunk> hybrid_or_throw(const docv1::Document& document,
                                            const parsev1::HybridChunkerOptions& options) {
  std::vector<parsev1::Chunk> chunks;
  const grpc::Status status = chunk_hybrid(document, {}, options, "doc.txt", &chunks);
  if (!status.ok()) throw std::runtime_error("chunk_hybrid failed: " + status.error_message());
  return chunks;
}

void verify_hybrid_chunks_use_the_hf_counter() {
  docv1::Document document = new_document();
  add_paragraph(&document, "hello world abc world");
  const auto options = hf_options(2, fixture_path());

  const auto chunks = hybrid_or_throw(document, options);
  require_eq(static_cast<int>(chunks.size()), 2, "four 1-token words pack two per chunk");
  require(chunks[0].text() == "hello world", "words pack greedily");
  require(chunks[1].text() == "abc world", "the tail keeps its words");
  const TokenCounter counter = load_fixture();
  for (const auto& chunk : chunks) {
    require_eq(chunk.num_tokens(), counter.count(chunk.text()),
               "num_tokens is the hf/1 count");
    require(chunk.rules_digest() ==
                "grparse-hybrid/1;tok=hf/1;sent=sentence/1;max_tokens=2;merge_peers=true",
            "the digest names the hf/1 counter: " + chunk.rules_digest());
  }

  // A run wordish/1 reads as one token merges into two under the fixture's
  // BPE: the counter, not just the label, changed.
  docv1::Document fused = new_document();
  add_paragraph(&fused, "helloworld");
  const auto hf_chunks = hybrid_or_throw(fused, hf_options(4, fixture_path()));
  require_eq(hf_chunks.front().num_tokens(), 2, "hf/1 sees hello + world");
  parsev1::HybridChunkerOptions wordish;
  wordish.set_max_tokens(4);
  const auto wordish_chunks = hybrid_or_throw(fused, wordish);
  require_eq(wordish_chunks.front().num_tokens(), 1, "wordish/1 sees one alphanumeric run");
  require(wordish_chunks.front().rules_digest().contains("tok=wordish/1"),
          "the default digest is unchanged");
}

void verify_the_hard_cut_measures_under_hf() {
  // One pretoken over budget: wordish/1 would cut by code point count, which
  // says nothing about a merge-heavy tokenizer, so hf/1 grows each piece one
  // code point at a time and measures.
  docv1::Document document = new_document();
  add_paragraph(&document, "helloworld");
  const auto chunks = hybrid_or_throw(document, hf_options(1, fixture_path()));
  require_eq(static_cast<int>(chunks.size()), 2, "hello and world each fit the budget alone");
  require(chunks[0].text() == "hello", "the cut lands on the merge boundary");
  require(chunks[1].text() == "world", "the second piece takes the rest");
  for (const auto& chunk : chunks) require(chunk.num_tokens() <= 1, "no piece over budget");
}

// Deterministic protobuf bytes, same as the chunking suite: the metadata map
// has no intrinsic order, so byte comparisons ask for the canonical one.
std::string serialized(const std::vector<parsev1::Chunk>& chunks) {
  parsev1::ChunkDocumentResponse response;
  for (const auto& chunk : chunks) *response.add_chunks() = chunk;
  std::string bytes;
  google::protobuf::io::StringOutputStream sink(&bytes);
  google::protobuf::io::CodedOutputStream stream(&sink);
  stream.SetSerializationDeterministic(true);
  if (!response.SerializeToCodedStream(&stream)) {
    throw std::runtime_error("chunk list serialization failed");
  }
  return bytes;
}

void verify_hf_chunking_is_byte_identical_across_runs_and_threads() {
  docv1::Document document = new_document();
  add_paragraph(&document, "hello world abc world.");
  add_paragraph(&document, "世界 世界 hello, world!");
  const auto options = hf_options(4, fixture_path());

  const std::string reference = serialized(hybrid_or_throw(document, options));
  require(!reference.empty(), "the fixture document must chunk to something");
  require(serialized(hybrid_or_throw(document, options)) == reference,
          "identical input chunks identically twice");

  std::vector<std::string> results(4);
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back(
        [&, worker] { results[static_cast<std::size_t>(worker)] = serialized(hybrid_or_throw(document, options)); });
  }
  for (auto& worker : workers) worker.join();
  for (int worker = 0; worker < 4; ++worker) {
    require(results[static_cast<std::size_t>(worker)] == reference,
            "concurrent hf/1 chunking is byte-identical (each call owns its counter)");
  }
}

}  // namespace

int main() {
  return grparse_test::run_test_main("chunk-hf-tokenizer-test", "all cases passed",
                                     {
                                         verify_resolution_order,
                                         verify_padding_and_truncation_are_stripped,
                                         verify_hf_counts_match_the_fixture_vocab,
                                         verify_load_failures_are_invalid_argument_not_crashes,
                                         verify_validation_accepts_hf_and_names_the_resolution_order,
                                         verify_hybrid_chunks_use_the_hf_counter,
                                         verify_the_hard_cut_measures_under_hf,
                                         verify_hf_chunking_is_byte_identical_across_runs_and_threads,
                                     });
}
