#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/enrich/v1/enrich_service.pb.h"
#include "grparse/collector_coordinator.h"

namespace grparse {

// The chart derender leg: raster charts the CV path only classified get
// their data table from grpc-enrich, the fleet's VLM face. gRParse never
// talks to a VLM itself; it sends the chart pictures (self_ref, verdict,
// pixels) to EnrichService.EnrichDocument with do_chart_extraction and folds
// each ChartTable that comes back into that picture's tabular-chart
// annotation, attributed to the model as a GenerationSource. Office charts
// already carry a typed table from their live model and are never sent.
//
// The leg is opt-in (GRPARSE_ENRICH_TARGET unset means it does not exist),
// bounded (the sooner of the inbound call's deadline and `timeout`), and
// advisory: a failure, a skip or a timeout leaves the document exactly as it
// was and counts as chart_derender_skipped; the parse never fails for it.
struct ChartDerenderOptions {
  // grpc-enrich's dial target; empty disables the leg.
  std::string target;
  // The ceiling on the whole leg (GRPARSE_ENRICH_TIMEOUT_MS). Forwarded to
  // the enrich service as its per-VLM-call timeout too, rounded up to whole
  // seconds, so the peer gives up when this side does.
  std::chrono::milliseconds timeout{5000};
  // Optional per-request VLM endpoint override (GRPARSE_ENRICH_VLM_ENDPOINT);
  // empty leaves the enrich service on its configured default.
  std::string vlm_endpoint;

  bool enabled() const { return !target.empty(); }
};

// One picture the leg would send: its arena index and self_ref plus the
// decoded pixels and their mimetype.
struct ChartCandidate {
  int picture_index = 0;
  std::string self_ref;
  std::string mimetype;
  std::string bytes;
};

// The pictures worth derendering: a classifier verdict (top predicted class)
// of bar_chart, line_chart or pie_chart, or the CHART label, with no
// tabular-chart annotation yet and inline pixels on the image reference
// (data: URI, base64). Pure; the order is arena order.
std::vector<ChartCandidate> chart_derender_candidates(
    const ai::pipestream::document::v1::Document& document);

// The request the leg sends: a Document holding only the candidate pictures
// (self_ref, label, classification annotations, provenance; image bytes
// stripped) so the enrich service selects exactly those, plus one ItemImage
// per candidate. Pure.
ai::pipestream::document::v1::Document chart_derender_request_document(
    const ai::pipestream::document::v1::Document& document,
    const std::vector<ChartCandidate>& candidates);

// Folds one ChartTable annotation into the picture whose self_ref it names
// (or, for a picture the arena never named, the one at the index in
// "#/pictures/<index>", the name chart_derender_candidates sent it under):
// a tabular_chart annotation (title, typed cells), the picture meta's
// tabular_chart field with created_by = model, and a GenerationSource on
// the picture naming the model (and the endpoint when known). Returns
// false, touching nothing, when no picture matches, the picture already
// carries a tabular chart, or the table is empty. Pure apart from the
// document it edits; applying the same annotation twice folds it once.
bool fold_chart_table(const ai::pipestream::enrich::v1::ItemAnnotation& annotation,
                      const std::string& endpoint,
                      ai::pipestream::document::v1::Document* document);

struct ChartDerenderReport {
  int candidates = 0;
  int derendered = 0;
  int skipped = 0;
  std::vector<std::string> warnings;
};

// Runs the leg over `document` through `channel`: selects, dials, folds,
// counts. Blocks for at most min(inbound_deadline, now + options.timeout).
// Never throws; a channel that is null or a target that fails to answer is
// reported as skipped candidates with one warning.
ChartDerenderReport derender_charts(const std::shared_ptr<grpc::Channel>& channel,
                                    const ChartDerenderOptions& options,
                                    ai::pipestream::document::v1::Document* document,
                                    CollectorDeadline inbound_deadline = kNoCollectorDeadline);

}  // namespace grparse
