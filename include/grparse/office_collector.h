#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "grparse/collector_coordinator.h"

namespace grparse {

// Streams one document's bytes to a grpc-libreoffice collector and folds
// its typed event stream into a source-tagged docling Document. Never
// throws: transport, load, and mapping failures land in the outcome so the
// coordinator can degrade instead of failing the parse.
CollectorOutcome collect_office_document(
    const std::shared_ptr<grpc::Channel>& channel, const std::string& document_id,
    const std::string& filename, const std::string& content_type,
    const std::string& bytes);

}  // namespace grparse
