#pragma once

// The parse the unary surfaces share: one FileSource in, a merged Document
// out. ConvertSource renders exports from it and the two chunkers chunk it,
// so the parse itself lives here rather than in any one of them.
//
// Internal to src: the served surfaces stay in
// include/grparse/document_parser_service.h.

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse.pb.h"
#include "chunking/chunker.h"
#include "grparse/chart_derender.h"
#include "grparse/chart_extraction_policy.h"
#include "grparse/collector_coordinator.h"
#include "grparse/document_parser_service.h"
#include "grparse/document_render.h"
#include "grparse/document_repair.h"
#include "grparse/inflight_bytes.h"
#include "grparse/page_scheduler.h"
#include "grparse/remote_fetch_slots.h"

namespace grparse {

// One parsed source: the merged document every conversion surface starts
// from, plus the offset table of its text stream.
struct SourceParse {
  std::filesystem::path filename;
  std::string_view doctype = "other";
  bool collectors_ran = false;
  CoordinatorResult result;
  chunking::OffsetTable offsets;
  // The CV collector's page read quality, when it read any page.
  std::optional<ai::pipestream::parse::v1::ConfidenceScores> confidence;
  // The structural rule findings, when the request asked for a REPORT.
  google::protobuf::RepeatedPtrField<ai::pipestream::parse::v1::StructureFinding>
      structure_findings;
  // A remote source's fetched copy, charged once the fetch returned; held
  // until the surface is done with this parse.
  InflightCharge fetched_charge;
};

// What a remote source is admitted under: the slots that bound the fetches
// running at once, and the in-flight budget the fetched copy is charged to.
// A null member leaves that bound off.
struct SourceAdmission {
  std::shared_ptr<InflightBytes> inflight;
  std::shared_ptr<RemoteFetchSlots> fetch_slots;
};

// The parse every unary surface shares: decode the single FileSource, plan
// the collectors, run them, and merge. The response shaping (exports, chunks)
// belongs to the caller. Blocking throughout, so it runs on a CallExecutor
// worker and never on the thread that reacted to the call; `context` outlives
// that worker because the reactor finishes the call from it.
//
// `surface` names the RPC in every rejection, so a caller learns which of
// the conversion surfaces turned its request down.
// Whether an S3Source's endpoint certificate is verified: true unless the
// request set verify_ssl to false explicitly.
bool s3_source_verify_ssl(const ai::pipestream::parse::v1::S3Source& source);

// An INVALID_ARGUMENT that names the rejected option: the message for
// people, and a typed OptionRejection packed in google.rpc.Status.details
// (the status's error_details) for programs.
grpc::Status option_rejected(const std::string& option_path,
                             ai::pipestream::parse::v1::OptionRejectionReason reason,
                             const std::string& rejected_value_raw, const std::string& message);

// The heading pass's numbering-scheme precedence, typed: numbering_schemes
// as given, or numbering_schemes_raw mapped by Docling name. Unspecified,
// unknown, repeated, or both-fields-set is INVALID_ARGUMENT naming the value.
std::expected<std::vector<ai::pipestream::parse::v1::HeadingNumberingScheme>, grpc::Status>
resolve_numbering_schemes(const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
                          const std::string& surface);

// The typed code/formula preset ConvertDocumentOptions.code_formula_preset
// names. Unset, empty and "default" resolve to Docling's default,
// CODEFORMULAV2; a name Docling does not register is INVALID_ARGUMENT.
std::expected<ai::pipestream::parse::v1::CodeFormulaPresetId, grpc::Status>
resolve_code_formula_preset(const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
                            const std::string& surface);

// The enrich service's typed preset for a resolved code/formula preset.
ai::pipestream::enrich::v1::CodeFormulaPreset enrich_code_formula_preset(
    ai::pipestream::parse::v1::CodeFormulaPresetId preset);

grpc::Status validate_options(const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
                              const std::string& surface);

// The bytes a request's sources decode to while the parse holds them: each
// FileSource's base64 payload decoded (three bytes per four characters).
// The unary surfaces charge it to the in-flight budget beside the wire size,
// since the decoded copy lives as long as the parse does.
uint64_t decoded_source_bytes(
    const google::protobuf::RepeatedPtrField<ai::pipestream::parse::v1::Source>& sources);

// The chart-extraction preset a Convert request resolves to under the
// server's policy, mirroring docling-jobkit's _parse_chart_extraction_options:
// a chart_extraction_custom_config must be allowed by policy (and use an
// allowed engine); otherwise the named chart_extraction_preset, or "default"
// when none is named, must be in the policy's registry. nullopt when the
// request names nothing and sets do_chart_extraction false. INVALID_ARGUMENT
// for an unknown or disallowed preset, PERMISSION_DENIED for a disabled
// custom config or a disallowed engine. Call after validate_options.
std::expected<std::optional<ChartExtractionPreset>, grpc::Status> resolve_chart_extraction(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
    const ChartExtractionPolicy& policy, const std::string& surface);

// The DocLang export options a request carries: image_export_mode (unset or
// UNSPECIFIED leaves each DocLang format at its own docling default) and
// doclang_include_namespace. validate_options has already turned down the
// combinations the renderers reject.
DoclangOptions doclang_options(const ai::pipestream::parse::v1::ConvertDocumentOptions& options);

// Docling's picture_description_api prompt, params and headers in the
// enrich service's typed form. The params map's names that have a typed
// field travel as one (model; max_tokens or its alias max_completion_tokens;
// temperature; top_p; seed), each checked for its type and range; any other
// name is refused by name. Headers become VlmHeader entries in name order;
// a name HTTP does not allow, one the HTTP client sets itself, two names
// differing only in case, or a value holding a control character is
// refused naming the header, never its value. INVALID_ARGUMENT on any
// refusal.
std::expected<PictureDescriptionCall, grpc::Status> picture_description_call(
    const ai::pipestream::parse::v1::PictureDescriptionApi& api, const std::string& surface);

// The request's picture-description call, resolved once per parse. With
// picture_description_api it carries the api's prompt, params and headers;
// without it, picture_description_custom_config's prompt (or, unset, its
// model_spec.prompt, as docling's from-spec options default it) is the
// prompt, and nothing else is set. grpc-enrich sends the headers to every
// per-request endpoint of the job, so headers alongside a chart-extraction
// preset that names a different endpoint, with the chart leg on, are
// INVALID_ARGUMENT rather than handed to that endpoint.
std::expected<PictureDescriptionCall, grpc::Status> request_picture_description_call(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options,
    const std::optional<ChartExtractionPreset>& chart_extraction, const std::string& surface);

// The model the picture-description leg names to grpc-enrich (its raw
// preset): picture_description_local.repo_id, else picture_description_preset,
// else picture_description_custom_config.model_spec.default_repo_id (docling's
// VlmModelSpec.get_repo_id with no engine override). Empty when none is set.
// model_spec.response_format is validated (a known value, agreeing with
// response_format_raw) and otherwise unread, as in docling, whose
// picture-description stage reads the answer as plain text whatever the spec
// names.
std::string picture_description_repo_id(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options);

// The identity parse_source stamps on every Document it returns
// (Document.parse): this build, its settings, and the request's options.
ai::pipestream::document::v1::ParseIdentity parse_identity(
    const ai::pipestream::parse::v1::ConvertDocumentOptions& options);

// ParseIdentity.options_digest for `options`: SHA-256 hex over the options
// that decide the Document (see document.proto for what is left out).
std::string options_digest(const ai::pipestream::parse::v1::ConvertDocumentOptions& options);

// ParseIdentity.settings_digest: the output-deciding server environment
// and the models MANIFEST, read once on first use.
const std::string& settings_digest();

// The same digest read afresh from the environment and the models
// directory; settings_digest() caches its first answer.
std::string read_settings_digest();

// A remote source is fetched holding one of `admission`'s fetch slots
// (waiting for one while the call is still wanted, CANCELLED once it is
// not), and its fetched copy is then charged to the in-flight budget before
// anything parses it: RESOURCE_EXHAUSTED when the budget refuses, else the
// charge rides `parsed->fetched_charge`.
grpc::Status parse_source(grpc::CallbackServerContext* context,
                          const ai::pipestream::parse::v1::ConvertDocumentRequest& request,
                          PageScheduler& scheduler,
                          const std::shared_ptr<CollectorEndpoints>& collectors,
                          const std::optional<RepairOptions>& server_repair,
                          const SourceAdmission& admission, const std::string& surface,
                          SourceParse* parsed);

}  // namespace grparse
