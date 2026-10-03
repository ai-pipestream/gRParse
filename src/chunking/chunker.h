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
// grparse-hier/2 serializes what list items hold (hier/1 dropped it);
// grparse-hybrid/2 builds on it and leaves a chunk whose heading trail
// alone reaches max_tokens unsplit (hybrid/1 cut it to 1-token pieces).
//
// The rule sets, in the exact spelling that reaches the wire:
//
//   "grparse-hier/2"   the hierarchical walk and the text serialization
//   "wordish/1"        the built-in token counter (token_counter.h)
//   "hf/1"             the HuggingFace tokenizer.json counter (token_counter.h)
//   "sentence/1"       the sentence splitter (sentence_rules.h)
//   "text/1"           the chunk text serialization, described below
//
// text/1 serializes one emitted unit as plain text:
//
//   text-family item  its text field, verbatim
//   list group        its items joined with "\n", each prefixed "- ", or
//                     "1. ", "2. ", ... for an ordered list; what an item
//                     holds follows its line: a nested list flattened in
//                     place with its own numbering, any other text item on
//                     a line of its own. A table or picture inside the list
//                     chunks on its own after the list.
//   table             its caption texts, then the rows flattened as
//                     "rowLabel, colLabel = value" triplets joined with ". ";
//                     a table with no headers, a single column, or nothing
//                     but headers degrades to its cell texts joined ". "
//   picture           its caption texts; with use_markdown_images, a
//                     placeholder line as well (default "![IMAGE]")
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
#include "token_counter.h"

namespace grparse::chunking {

// One text item's place in the document's concatenated text stream
// (furniture included), in UTF-8 code points. The CV path records it while
// it assembles pages (PageData.text_offsets); every other document gets it
// from derive_offsets. A table that does not name an item leaves the chunks
// holding that item without offsets rather than with invented ones.
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

// The offset table of a finished document's text stream: every text item in
// arena order, each joined to the one before it by a single newline, which
// is exactly the plain-text export (OUTPUT_FORMAT_TEXT). An item with no
// text arm set has no row and adds no separator. Rows carry
// TEXT_SOURCE_UNSPECIFIED: only the CV path knows how its text was read.
OffsetTable derive_offsets(const ai::pipestream::document::v1::Document& document);

// Copies how the CV path read each item (digital or OCR) from its own rows onto the derived table. A row lands only where the table names
// the same item with the same span; any other row describes text the
// document no longer holds and is ignored, so a mismatch costs the label,
// never the offsets.
void overlay_sources(const google::protobuf::RepeatedPtrField<
                         ai::pipestream::parse::v1::TextOffset>& rows,
                     OffsetTable* table);

// The table's rows in stream order (ascending start, then self_ref), the
// wire form a response carries.
google::protobuf::RepeatedPtrField<ai::pipestream::parse::v1::TextOffset> offset_rows(
    const OffsetTable& table);

// The rules_digest a hierarchical chunk carries.
inline constexpr std::string_view kHierarchicalRules = "grparse-hier/2";

// The rules_digest a hybrid chunk carries: the hybrid rule set plus every
// input that can move a boundary. Exactly
// "grparse-hybrid/2;tok=T;sent=sentence/1;max_tokens=N;merge_peers=B"
// with T the counter's rules ("wordish/1" or "hf/1"), N in decimal, and B
// spelled "true" or "false". Note the digest names the counter, not the
// tokenizer.json hf/1 loaded; the file is a deployment's model, and two
// deployments with different files chunk differently under the same digest.
std::string hybrid_rules_digest(int max_tokens, bool merge_peers,
                                std::string_view tokenizer = kTokenizerRules);

// The serialization options both chunkers share.
struct ChunkOptions {
  // Serialize tables as pipe-delimited Markdown (a header row, a separator
  // row, then the body rows, with "|" inside a cell escaped as "\|") instead
  // of the default triplet flattening.
  bool use_markdown_tables = false;
  // Populate Chunk.raw_text. It repeats Chunk.text today and is reserved for
  // a future richer serialization of the same chunk.
  bool include_raw_text = false;
  // When true, picture chunks include an image placeholder line (parity with
  // docling-jobkit MarkdownChunkingSerializerProvider.use_markdown_images).
  // Uncaptioned pictures then still emit a chunk.
  bool use_markdown_images = false;
  // Placeholder text for pictures when use_markdown_images is set. Empty
  // means the jobkit default "![IMAGE]" unless image_placeholder_set is true
  // (explicit empty string from the wire).
  std::string image_placeholder;
  bool image_placeholder_set = false;
};

// Build shared serialization options from the hierarchical / hybrid wire
// messages (markdown tables/images + raw_text).
ChunkOptions chunk_options_from(
    const ai::pipestream::parse::v1::HierarchicalChunkerOptions& options);
ChunkOptions chunk_options_from(
    const ai::pipestream::parse::v1::HybridChunkerOptions& options);

// Fill ChunkDocumentResponse.chunking_info from the active chunker options
// (parity with jobkit ChunkedDocumentResult.chunking_info = options.model_dump).
void fill_chunking_info(
    const ai::pipestream::parse::v1::HierarchicalChunkerOptions& options,
    google::protobuf::Map<std::string, ai::pipestream::parse::v1::ScalarValue>* out);
void fill_chunking_info(
    const ai::pipestream::parse::v1::HybridChunkerOptions& options,
    google::protobuf::Map<std::string, ai::pipestream::parse::v1::ScalarValue>* out);

// The hierarchical chunker: one chunk per emitted unit in body-tree walk
// order, each carrying the heading trail in force where it was emitted.
std::vector<ai::pipestream::parse::v1::Chunk> chunk_hierarchical(
    const ai::pipestream::document::v1::Document& document,
    const OffsetTable& offsets, const ChunkOptions& options,
    std::string_view filename);

// Rejects a hybrid request whose options cannot produce a deterministic
// chunking: max_tokens is required, and the optional tokenizer field must
// name one of the tokenizers this service implements ("wordish/1", the
// default, or "hf/1"). hf/1 additionally resolves and loads its
// tokenizer.json here (see token_counter.h for the resolution order), so a
// request whose file is absent fails before any parsing work starts.
grpc::Status validate_hybrid_options(
    const ai::pipestream::parse::v1::HybridChunkerOptions& options);

// The hybrid chunker: the hierarchical chunks, then peer merging under the
// token budget, then a sentence-wise split of anything still over it.
// `options` must have passed validate_hybrid_options; a counter that still
// fails to load (its file changed after validation) is an INTERNAL error
// rather than a silent fallback to wordish/1 counts.
grpc::Status chunk_hybrid(
    const ai::pipestream::document::v1::Document& document,
    const OffsetTable& offsets,
    const ai::pipestream::parse::v1::HybridChunkerOptions& options,
    std::string_view filename,
    std::vector<ai::pipestream::parse::v1::Chunk>* out);

}  // namespace grparse::chunking

#endif
