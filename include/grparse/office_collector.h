#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "grparse/collector_coordinator.h"
#include "grparse/document_passwords.h"
#include "grparse/office_cv_enrichment.h"

namespace grparse {

// Streams one document's bytes to a grpc-libreoffice collector and folds
// its typed event stream into a source-tagged docling Document. Never
// throws: transport, load, and mapping failures land in the outcome so the
// coordinator can degrade instead of failing the parse.
//
// When an enrichment with a detector is supplied, the mapped document's page
// renders additionally run through the CV engines (layout, figure classes,
// barcodes) before the outcome returns; see office_cv_enrichment.h.
//
// `inbound_deadline` is the absolute ceiling of the call that asked for the
// parse: the stream runs until the sooner of that and this leg's own static
// cap. kNoCollectorDeadline (the default) means the call carried none, which
// leaves the leg on its cap alone. `cancelled` cancels the stream once the
// inbound call is gone; empty (the default) watches nothing.
//
// `passwords` are the call's candidates for an encrypted document; they
// ride the collector call's metadata, never its payload. A password-
// protected document the collector cannot open fails INVALID_ARGUMENT with
// the backend contract's LOAD_STATUS_PASSWORD_REQUIRED token, the verdict
// an encrypted PDF gets, saying how many candidates were tried.
CollectorOutcome collect_office_document(
    const std::shared_ptr<grpc::Channel>& channel, const std::string& document_id,
    const std::string& filename, const std::string& content_type,
    const std::string& bytes, const OfficeCvEnrichment& enrichment = {},
    CollectorDeadline inbound_deadline = kNoCollectorDeadline,
    CollectorCancelled cancelled = {}, const DocumentPasswords& passwords = {});

}  // namespace grparse
