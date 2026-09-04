// The hf/1 token counter against a real HuggingFace tokenizer.json from the
// deployment's models directory (models/chunk/tokenizer.json, for example
// all-MiniLM-L6-v2, see models/README.md). The fixture-sized cases live in
// chunk_hf_tokenizer_test.cpp; this tier proves the loader and the counter
// against a full vocabulary. Without the file it exits 77, which ctest
// counts as a skip, matching the other model-gated tiers.
#include <cstdlib>
#include <filesystem>
#include <print>
#include <string>
#include <vector>

#include "../src/chunking/chunker.h"
#include "../src/chunking/token_counter.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;

namespace fs = std::filesystem;

namespace {

constexpr int kSkipExitCode = 77;
using grparse_test::require;

void verify_counts_against_a_real_vocabulary(const fs::path& model) {
  grparse::chunking::TokenCounter counter;
  const grpc::Status loaded =
      grparse::chunking::TokenCounter::huggingface(model.string(), &counter);
  require(loaded.ok(), "the deployment tokenizer.json must load: " + loaded.error_message());
  require(counter.rules() == "hf/1", "the counter reports its rule set");

  // Invariants, not golden counts: the tier must hold for whichever real
  // tokenizer.json the deployment drops in.
  const int hello_world = counter.count("hello world");
  require(hello_world >= 2 && hello_world <= 8,
          "two plain English words count as a handful of tokens, got " +
              std::to_string(hello_world));
  require(counter.count("hello world") == hello_world, "the count is stable");
  require(counter.count("") == 0, "empty text counts zero");
  // A run long enough that any real fixed padding or truncation the file
  // shipped would have betrayed itself by capping the count.
  std::string long_text;
  for (int i = 0; i < 300; ++i) long_text += "hello ";
  require(counter.count(long_text) > 128,
          "a 300-word run must count well past every MiniLM-class window");
}

void verify_end_to_end_chunking_via_models_dir(const fs::path& models_dir) {
  // Resolve the way the server resolves: no request path, no
  // GRPARSE_CHUNK_TOKENIZER, GRPARSE_MODELS_DIR naming the directory.
  setenv("GRPARSE_MODELS_DIR", models_dir.string().c_str(), 1);
  unsetenv("GRPARSE_CHUNK_TOKENIZER");

  parsev1::HybridChunkerOptions options;
  options.set_max_tokens(32);
  options.set_tokenizer("hf/1");
  require(grparse::chunking::validate_hybrid_options(options).ok(),
          "a deployment tokenizer.json under the models dir validates");

  docv1::Document document;
  document.set_schema_name("docling_document_v2");
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  std::string text;
  for (int i = 0; i < 60; ++i) text += "The quick brown fox jumps over the lazy dog. ";
  auto* base = document.add_texts()->mutable_text()->mutable_base();
  base->set_self_ref("#/texts/0");
  base->set_text(text);
  base->set_orig(text);
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->mutable_parent()->set_ref("#/body");
  document.mutable_body()->add_children()->set_ref("#/texts/0");

  const auto chunk_once = [&] {
    std::vector<parsev1::Chunk> chunks;
    const grpc::Status status =
        grparse::chunking::chunk_hybrid(document, {}, options, "doc.txt", &chunks);
    require(status.ok(), "chunk_hybrid with hf/1 failed: " + status.error_message());
    return chunks;
  };
  const std::vector<parsev1::Chunk> chunks = chunk_once();
  require(chunks.size() > 1, "sixty repetitions cannot fit one 32-token chunk");
  for (const auto& chunk : chunks) {
    require(chunk.num_tokens() <= 32, "every piece respects the budget");
    require(chunk.rules_digest().contains("tok=hf/1"), "the digest names the counter");
  }
  const auto again = chunk_once();
  require(again.size() == chunks.size(), "the run is deterministic");
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    require(again[i].text() == chunks[i].text() && again[i].num_tokens() == chunks[i].num_tokens(),
            "identical input gives identical boundaries and counts");
  }
}

}  // namespace

int main() {
  try {
    const char* models_env = std::getenv("GRPARSE_TEST_MODELS_DIR");
    const fs::path models_dir = models_env == nullptr ? "models" : models_env;
    const fs::path model = models_dir / "chunk" / "tokenizer.json";
    if (!fs::exists(model)) {
      std::println(stderr, "chunk-hf-model-test: skipped, model not present: {:?}",
                   model.string());
      return kSkipExitCode;
    }
    verify_counts_against_a_real_vocabulary(model);
    verify_end_to_end_chunking_via_models_dir(models_dir);
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "chunk-hf-model-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
