#pragma once

// What the two parsing surfaces share. The unary surface (ConvertSource and
// the chunkers) and the streaming one run different machinery around the
// same middle: the same collector legs, the same recognition tuning, the
// same mapping from a thrown exception to a status. That middle lives here
// so neither surface owns it.
//
// Internal to src: the served surfaces stay in
// include/grparse/document_parser_service.h.

#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "grparse/collector_coordinator.h"
#include "grparse/document_parser_service.h"
#include "grparse/page_scheduler.h"

namespace grparse {

// The document's identity hash, stamped on the origin: FNV-1a over the
// bytes as they arrived.
uint64_t content_hash(const std::string& document);

// True when the bytes or the name say PDF. The bytes are asked first
// because a name is a claim and a signature is evidence.
bool is_pdf(const std::string& content, const std::filesystem::path& filename);

// True for every collector run_remote_collector can dial.
bool remote_collector(ai::pipestream::parse::v1::Collector id);

// True for the collectors that parse in this process instead of over a
// channel. The CV pipeline is in process too but keeps its own path: it is
// page-streamed and tunable, while these are a straight bytes-in,
// Document-out fold with nothing to configure and nothing to reach.
bool local_collector(ai::pipestream::parse::v1::Collector id);

// Runs one in-process collector. Never throws; failures are outcomes, so a
// local collector degrades exactly like a dialed one.
CollectorOutcome run_local_collector(ai::pipestream::parse::v1::Collector id,
                                     const std::string& bytes);

// Dials one Document-emitting remote collector and returns its outcome.
// Configuration failures are outcomes too, so the parse degrades collector
// by collector no matter where the failure sits. The office collector keeps
// its own path: it streams typed events for gRParse to fold and enrich.
//
// `inbound_deadline` is the deadline of the call that asked for the parse,
// threaded down so no leg outlives the client waiting on it; each leg still
// caps itself at its own ceiling, and kNoCollectorDeadline (an inbound call
// with no deadline of its own) leaves every leg on that ceiling alone.
CollectorOutcome run_remote_collector(
    ai::pipestream::parse::v1::Collector id,
    const std::shared_ptr<CollectorEndpoints>& endpoints,
    const std::string& document_id, const std::string& filename,
    const std::string& content_type, const std::string& bytes,
    const std::string& ebcdic_layout_json,
    const std::string& lol_html_options_json,
    CollectorDeadline inbound_deadline);

// Validation both surfaces share: the unary options message and the
// streaming chunk carry the same recognition fields with the same rules.
grpc::Status validate_ocr_tuning(bool has_do_ocr, bool do_ocr, bool force_ocr,
                                 bool has_render_scale, double render_scale);

// The scheduler tuning the validated recognition fields resolve to. The
// options steer only the in-process CV collector; remote collectors read
// their own inputs and never see them.
PageScheduler::OcrTuning ocr_tuning(bool has_do_ocr, bool do_ocr, bool force_ocr,
                                    bool has_render_scale, double render_scale);

// The status a thrown failure reports as: each exception this pipeline
// raises has one status class, and everything else is INTERNAL.
grpc::Status status_from_exception(std::exception_ptr failure);

// The cancellation outcome every collector leg reports when the call it
// belongs to died before the leg started.
CollectorOutcome cancelled_outcome();

}  // namespace grparse
