#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <grpcpp/support/status.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/markup/v1/markup.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"

namespace grparse {

// The absolute wall-clock ceiling a collector leg may not outlive, threaded
// down from the inbound call's own deadline. Absolute rather than a
// duration on purpose: a call can sit in the executor queue for as long as
// the queue is deep, and only an absolute instant survives that wait
// unchanged.
using CollectorDeadline = std::chrono::system_clock::time_point;

// "The call carried no deadline", which leaves every leg on its own static
// cap. It is the default everywhere a deadline is threaded, so a caller that
// has none to give keeps exactly today's behaviour.
inline constexpr CollectorDeadline kNoCollectorDeadline = CollectorDeadline::max();

// The deadline one leg's ClientContext gets: the sooner of the inbound
// ceiling and the leg's own cap measured from now. A leg never outlives the
// client that asked for it, and never runs past its cap when the client is
// more patient than the cap is.
CollectorDeadline capped_collector_deadline(CollectorDeadline inbound,
                                            std::chrono::system_clock::duration cap);

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
// The wire name of a collector, as its items' CollectorSource spells it.
const char* collector_name(ai::pipestream::parse::v1::Collector collector);

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
// or the application/warc content type) to fastwarc, the wiki storage
// dialect to the in-process storage handler, audio and video to asr,
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
