#pragma once

// What a conversion carries beside its Document: the exports and the chunks
// the options asked for, the structure findings, the CV read quality and
// the offset table. ConvertSource answers with them on
// ConvertDocumentResponse and StreamDocument on StreamSourceOutputs; both
// fill their message through fill_conversion_outputs, so the two surfaces
// cannot drift apart.
//
// Internal to src: the served surfaces stay in
// include/grparse/document_parser_service.h.

#include <string_view>
#include <utility>

#include <google/protobuf/repeated_field.h>
#include <grpcpp/grpcpp.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "chunking/chunker.h"
#include "source_parse.h"

namespace grparse {

// True when a rendering left something of the document out. A
// RenderOmissions that counts nothing has every field at its default and
// serializes to no bytes.
bool left_something_out(const ai::pipestream::parse::v1::RenderOmissions& omissions);

// Renders every output format the options asked for onto `exports`. An
// empty to_formats keeps the historical default of the plain-text export
// alone. A renderer that lays tables out as grids bounds what they may cost
// (grparse/document_render.h); whatever it left out for that goes on
// `exports` too, one ExportOmissions entry per format, so a cut export is
// never returned as a complete one. TEXT, JSON, YAML and VTT build no grid
// and have nothing to report.
void render_exports(const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
                    const ai::pipestream::document::v1::Document& document,
                    ai::pipestream::parse::v1::DocumentExports* exports);

// The chunks OUTPUT_FORMAT_CHUNKS in to_formats asks for, appended to
// `chunks`: the hybrid chunker's when the options carry hybrid_chunking,
// the hierarchical one's otherwise. Nothing when the format is not asked
// for. A hybrid chunker that cannot run is the returned status. What the
// chunker left out of the document (chunking/chunker.h) lands on
// `omissions`, which stays untouched when the chunks hold everything it
// serializes.
grpc::Status render_chunks(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
    const ai::pipestream::document::v1::Document& document,
    const chunking::OffsetTable& offsets, std::string_view filename,
    google::protobuf::RepeatedPtrField<ai::pipestream::parse::v1::Chunk>* chunks,
    ai::pipestream::parse::v1::RenderOmissions* omissions);

// Writes what the conversion of `parsed` carries beside `document`, its
// merged Document: onto `outputs` the CV read quality when a CV page was
// read, the structure findings (taken out of `parsed`), the offset table,
// the chunks and, when they are not the whole document, what they left
// out; onto `exports` every requested format. `Outputs` is
// ConvertDocumentResponse or StreamSourceOutputs, which name these fields
// alike; the exports are passed apart because the unary response nests
// them under its document. A chunker's failure is the returned status and
// a renderer's is thrown, with `outputs` left partly written either way.
template <typename Outputs>
grpc::Status fill_conversion_outputs(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
    const ai::pipestream::document::v1::Document& document, SourceParse* parsed,
    ai::pipestream::parse::v1::DocumentExports* exports, Outputs* outputs) {
  if (parsed->confidence.has_value()) *outputs->mutable_confidence() = *parsed->confidence;
  outputs->mutable_structure_findings()->Swap(&parsed->structure_findings);
  *outputs->mutable_text_offsets() = chunking::offset_rows(parsed->offsets);
  // Every requested output format renders from the same merged document;
  // TEXT keeps its arena-order line export, the rest fold the body tree.
  render_exports(options, document, exports);
  ai::pipestream::parse::v1::RenderOmissions chunk_omissions;
  const grpc::Status chunked =
      render_chunks(options, document, parsed->offsets, parsed->filename.string(),
                    outputs->mutable_chunks(), &chunk_omissions);
  if (left_something_out(chunk_omissions)) {
    *outputs->mutable_chunk_omissions() = std::move(chunk_omissions);
  }
  return chunked;
}

}  // namespace grparse
