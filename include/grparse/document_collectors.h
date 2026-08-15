#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "grparse/collector_coordinator.h"

namespace grparse {

// Clients for the collectors that project their own typed stream into a
// Document server-side (TranscribeOptions.emit_document and friends). Each
// streams the input bytes up, asks for the Document event, and returns it
// as the outcome; the collector's typed events are drained and dropped
// because the fold already happened where the events were made. None of
// these throw: transport and collector failures land in the outcome so the
// coordinator can degrade instead of failing the parse.

// grpc-asr. `model` is the whisper model name the collector must have
// loaded; the caller resolves it from configuration, never guesses.
CollectorOutcome collect_asr_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& model,
                                      const std::string& bytes);

// grpc-email (.eml / .msg bytes).
CollectorOutcome collect_email_document(const std::shared_ptr<grpc::Channel>& channel,
                                        const std::string& document_id,
                                        const std::string& filename,
                                        const std::string& content_type,
                                        const std::string& bytes);

// grpc-xml. The dialect is left unspecified so the collector sniffs it; a
// sniff that fails is that collector's failure, not the parse's.
CollectorOutcome collect_xml_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& bytes);

// grpc-ebcdic. `layout_json` is the JSON serialization of Docling's
// EbcdicLayout, forwarded verbatim; empty is a caller error surfaced as an
// INVALID_ARGUMENT outcome before anything is dialed.
CollectorOutcome collect_ebcdic_document(const std::shared_ptr<grpc::Channel>& channel,
                                         const std::string& layout_json,
                                         const std::string& bytes);

// grpc-epub (.epub archive bytes).
CollectorOutcome collect_epub_document(const std::shared_ptr<grpc::Channel>& channel,
                                       const std::string& bytes);

// grpc-markup (Markdown, HTML, AsciiDoc, LaTeX, WebVTT, BoxNote, Docling
// JSON). The format is hinted from the filename and content type via
// markup_format_for; an unresolved hint asks the collector to sniff.
CollectorOutcome collect_markup_document(const std::shared_ptr<grpc::Channel>& channel,
                                         const std::string& filename,
                                         const std::string& content_type,
                                         const std::string& bytes);

}  // namespace grparse
