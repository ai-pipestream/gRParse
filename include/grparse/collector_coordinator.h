#pragma once

#include <functional>
#include <string>
#include <vector>

#include <grpcpp/support/status.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/markup/v1/markup.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"

namespace grparse {

// One collector's complete output: its projected Document (source-tagged by
// the collector itself) or the reason it could not contribute.
struct CollectorOutcome {
  bool success = false;
  // Failure detail and its gRPC mapping, meaningful when success is false.
  // The code survives so a parse whose only collector fails can report the
  // collector's own status class instead of a generic INTERNAL.
  std::string error;
  grpc::StatusCode code = grpc::StatusCode::INTERNAL;
  // The collector's extraction warnings, verbatim.
  std::vector<std::string> warnings;
  ai::pipestream::document::v1::Document document;
};

// One collector planned into a parse: its wire identity and the callable
// that produces its outcome. The callable must not throw; failures are
// outcomes.
struct PlannedCollector {
  ai::pipestream::parse::v1::Collector id =
      ai::pipestream::parse::v1::COLLECTOR_UNSPECIFIED;
  std::function<CollectorOutcome()> run;
};

// One collector that could not contribute to an otherwise surviving parse.
struct CollectorFailureInfo {
  ai::pipestream::parse::v1::Collector id =
      ai::pipestream::parse::v1::COLLECTOR_UNSPECIFIED;
  std::string error;
  grpc::StatusCode code = grpc::StatusCode::INTERNAL;
};

// The scatter-gather result: every successful collector's output merged
// additively, plus the failures and per-collector warnings of the run.
struct CoordinatorResult {
  ai::pipestream::document::v1::Document document;
  std::vector<CollectorFailureInfo> failures;
  std::vector<std::pair<ai::pipestream::parse::v1::Collector, std::string>> warnings;
  int succeeded = 0;
};

// Runs every planned collector concurrently and merges the successful
// outputs additively into `base` in plan order, so the merged document is
// deterministic regardless of finish order. A collector that fails becomes
// a failure entry; it never sinks the parse while another collector
// succeeds.
CoordinatorResult run_collectors(
    std::vector<PlannedCollector> collectors,
    ai::pipestream::document::v1::Document base);

// True when the filename extension or the content type names an office
// format the libreoffice collector owns.
bool office_format(const std::string& filename, const std::string& content_type);

// The collector a document routes to when the caller selects none: office
// formats to libreoffice, WARC archives (.warc and its gzip/zstd/lz4 forms,
// or the application/warc content type) to fastwarc, audio and video to asr,
// .eml/.msg to email, XML
// and its archive forms (.dclx, .tar.gz METS exports) to xml, .epub to
// epub, text markup (Markdown, HTML, AsciiDoc, LaTeX, WebVTT, BoxNote,
// JSON) to markup, everything else (PDF, raster) to the in-process CV
// path. EBCDIC never routes by format: raw records have no extension or
// magic worth trusting, and a parse needs a layout only an explicit caller
// can supply.
ai::pipestream::parse::v1::Collector route_collector(const std::string& filename,
                                                     const std::string& content_type);

// The format hint the markup collector is dialed with, from the filename
// extension and content type. `MARKUP_FORMAT_UNSPECIFIED` when neither
// resolves one, which asks the collector to sniff the bytes itself.
ai::pipestream::markup::v1::MarkupFormat markup_format_for(
    const std::string& filename, const std::string& content_type);

// Resolves the collector plan for a document: an explicit selection wins
// verbatim (unspecified entries and duplicates dropped, order kept); an
// empty selection becomes the routed default.
std::vector<ai::pipestream::parse::v1::Collector> resolve_collectors(
    const std::vector<ai::pipestream::parse::v1::Collector>& requested,
    ai::pipestream::parse::v1::Collector routed);

}  // namespace grparse
