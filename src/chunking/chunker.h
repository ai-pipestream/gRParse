// The document chunkers behind ChunkHierarchicalSource and ChunkHybridSource.
// Internal seam of the chunking module (src/chunking/*); the service is the
// only caller besides the tests.
//
// Determinism is the product. Identical input bytes must produce identical
// chunk bytes forever, across runs, threads, and machines, so every boundary
// decision here is a pure function of the parsed Document plus the request's
// own options: no clock, no locale, no randomness, no downloaded tokenizer,
// no floating-point default. The rule sets are versioned and every chunk
// carries the version it was produced under in its rules_digest, which makes
// a boundary change a visible wire change instead of a silent reshuffle.
//
// The rule sets, in the exact spelling that reaches the wire:
//
//   "grparse-hier/1"   the hierarchical walk and the text serialization
//   "wordish/1"        the token counter (token_counter.h)
//   "sentence/1"       the sentence splitter (sentence_rules.h)
//   "text/1"           the chunk text serialization, described below
//
// text/1 serializes one emitted unit as plain text:
//
//   text-family item  its text field, verbatim
//   list group        its items joined with "\n", each prefixed "- ", or
//                     "1. ", "2. ", ... for an ordered list
//   table             its caption texts, then the rows flattened as
//                     "rowLabel, colLabel = value" triplets joined with ". ";
//                     a table with no headers, a single column, or nothing
//                     but headers degrades to its cell texts joined ". "
//   picture           its caption texts only, never a placeholder
//   code              its text, verbatim
//
// An emitted unit whose serialization is blank produces no chunk at all.
#ifndef GRPARSE_CHUNKING_CHUNKER_H
#define GRPARSE_CHUNKING_CHUNKER_H

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <grpcpp/support/status.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"

namespace grparse::chunking {

// One text item's place in the document's concatenated text stream
// (furniture included), in UTF-8 code points. This is the parse's own side
// table (PageData.text_offsets), not something the chunker can derive: a
// document that arrives without one yields chunks with no offsets rather
// than invented ones.
struct OffsetEntry {
  std::uint64_t start = 0;
  std::uint64_t end = 0;
  ai::pipestream::parse::v1::TextSource source =
      ai::pipestream::parse::v1::TEXT_SOURCE_UNSPECIFIED;
};

// The offset side table, keyed by the item's self_ref.
using OffsetTable = std::map<std::string, OffsetEntry>;

// Folds a page's offset rows into the table. Rows whose self_ref is already
// present are ignored, which keeps the first page that claimed a reference
// the owner of it.
void add_offsets(const google::protobuf::RepeatedPtrField<
                     ai::pipestream::parse::v1::TextOffset>& rows,
                 OffsetTable* table);

// The rules_digest a hierarchical chunk carries.
inline constexpr std::string_view kHierarchicalRules = "grparse-hier/1";

// The rules_digest a hybrid chunk carries: the hybrid rule set plus every
// input that can move a boundary. Exactly
// "grparse-hybrid/1;tok=wordish/1;sent=sentence/1;max_tokens=N;merge_peers=B"
// with N in decimal and B spelled "true" or "false".
std::string hybrid_rules_digest(int max_tokens, bool merge_peers);

// The serialization options both chunkers share.
struct ChunkOptions {
  // Serialize tables as pipe-delimited Markdown (a header row, a separator
  // row, then the body rows, with "|" inside a cell escaped as "\|") instead
  // of the default triplet flattening.
  bool use_markdown_tables = false;
  // Populate Chunk.raw_text. It repeats Chunk.text today and is reserved for
  // a future richer serialization of the same chunk.
  bool include_raw_text = false;
};

// The hierarchical chunker: one chunk per emitted unit in body-tree walk
// order, each carrying the heading trail in force where it was emitted.
std::vector<ai::pipestream::parse::v1::Chunk> chunk_hierarchical(
    const ai::pipestream::document::v1::Document& document,
    const OffsetTable& offsets, const ChunkOptions& options,
    std::string_view filename);

// Rejects a hybrid request whose options cannot produce a deterministic
// chunking: max_tokens is required, and the optional tokenizer field must
// name the one tokenizer this service implements.
grpc::Status validate_hybrid_options(
    const ai::pipestream::parse::v1::HybridChunkerOptions& options);

// The hybrid chunker: the hierarchical chunks, then peer merging under the
// token budget, then a sentence-wise split of anything still over it.
// `options` must have passed validate_hybrid_options.
std::vector<ai::pipestream::parse::v1::Chunk> chunk_hybrid(
    const ai::pipestream::document::v1::Document& document,
    const OffsetTable& offsets,
    const ai::pipestream::parse::v1::HybridChunkerOptions& options,
    std::string_view filename);

}  // namespace grparse::chunking

#endif
