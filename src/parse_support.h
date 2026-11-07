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
#include <optional>
#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/ebcdic/v1/ebcdic.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "grparse/collector_coordinator.h"
#include "grparse/document_parser_service.h"
#include "grparse/document_passwords.h"
#include "grparse/page_scheduler.h"
#include "lolhtml/v1/lolhtml_service.pb.h"

namespace grparse {

// The document's identity hash, stamped on the origin: FNV-1a over the
// bytes as they arrived.
uint64_t content_hash(const std::string& document);

// content_hash fed piecewise: update() with consecutive pieces of the
// document, and value() is content_hash of the pieces concatenated. The
// streaming surface feeds each chunk as it lands, so the hash is ready when
// the upload ends instead of costing another pass over it.
class ContentHash {
 public:
  void update(std::string_view piece) {
    for (const unsigned char byte : piece) {
      hash_ ^= byte;
      hash_ *= 1099511628211ULL;
    }
  }
  uint64_t value() const { return hash_; }

 private:
  uint64_t hash_ = 14695981039346656037ULL;
};

// True when the bytes or the name say PDF. The bytes are asked first
// because a name is a claim and a signature is evidence: a %PDF- header in
// the first kilobyte is a PDF whatever its name, bytes that sniff as any
// other type are not one, and only bytes that say nothing leave it to a
// case-insensitive .pdf extension.
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

// The rules a request carries for the collectors that cannot run without
// them, in each collector's own typed form. The ebcdic options hold no
// layout and lol_html is empty when the request gave none; that collector's
// leg then fails before dialing.
struct CollectorRules {
  ai::pipestream::ebcdic::v1::ParseOptions ebcdic;
  std::optional<lolhtml::v1::ExtractOptions> lol_html;
  // Typed ASR options forwarded to grpc-asr when the ASR collector runs.
  // Empty keeps today's GRPARSE_ASR_MODEL / GRPARSE_ASR_DIARIZE defaults.
  std::optional<ai::pipestream::parse::v1::AsrPipelineOptions> asr_options;
  // The call's candidate passwords, for the collector that opens an
  // encrypted document itself (libreoffice). Never logged.
  DocumentPasswords passwords;
};

// Dials one Document-emitting remote collector and returns its outcome.
// Configuration failures are outcomes too, so the parse degrades collector
// by collector no matter where the failure sits. The office collector keeps
// its own path: it streams typed events for gRParse to fold and enrich.
//
// `inbound_deadline` is the deadline of the call that asked for the parse,
// threaded down so no leg outlives the client waiting on it; each leg still
// caps itself at its own ceiling, and kNoCollectorDeadline (an inbound call
// with no deadline of its own) leaves every leg on that ceiling alone.
// `cancelled` reaches the collector client, which cancels its call once the
// inbound call is gone.
CollectorOutcome run_remote_collector(
    ai::pipestream::parse::v1::Collector id,
    const std::shared_ptr<CollectorEndpoints>& endpoints,
    const std::string& document_id, const std::string& filename,
    const std::string& content_type, const std::string& bytes, const CollectorRules& rules,
    CollectorDeadline inbound_deadline, CollectorCancelled cancelled = {});

// Validation both surfaces share: the unary options message and the
// streaming chunk carry the same recognition fields with the same rules.
grpc::Status validate_ocr_tuning(bool has_do_ocr, bool do_ocr, bool force_ocr,
                                 bool has_render_scale, double render_scale);

// The scheduler tuning the validated recognition fields resolve to. The
// options steer only the in-process CV collector; remote collectors read
// their own inputs and never see them.
PageScheduler::OcrTuning ocr_tuning(bool has_do_ocr, bool do_ocr, bool force_ocr,
                                    bool has_render_scale, double render_scale);

// Resolves replace_broken_text_layer from ocr_options.*.base. Unset means unset.
// Unsupported engine arms are UNIMPLEMENTED.
grpc::Status resolve_replace_broken_text_layer(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
    std::optional<bool>* out);

// Resolves rapidocr.detect_orientation from the rapidocr arm. Unsupported
// engine arms are UNIMPLEMENTED.
grpc::Status resolve_rapidocr_detect_orientation(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
    std::optional<bool>* out);



// The status a thrown failure reports as: each exception this pipeline
// raises has one status class, and everything else is INTERNAL.
grpc::Status status_from_exception(std::exception_ptr failure);

// A collector leg's failed outcome, from the exception that ended it,
// carrying the status status_from_exception maps the throw to.
CollectorOutcome outcome_from_exception(std::exception_ptr failure);

// The cancellation outcome every collector leg reports when the call it
// belongs to died before the leg started.
CollectorOutcome cancelled_outcome();

}  // namespace grparse
