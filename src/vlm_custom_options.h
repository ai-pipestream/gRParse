#pragma once

// Checks for the VLM custom-config fields that docling-serve carries twice,
// as a closed enum and as the raw string it was parsed from: the Nemotron
// Parse multi-pass options and an engine override's torch dtype.
//
// The rule is the same for every such pair. A typed value must be a member
// of its enum. A raw string that names a member must name the same member as
// a typed value set beside it. A raw string that names no member is kept for
// the caller while the typed value is unset, unless docling's own field is
// closed (Nemotron's pass names are a Literal), which makes it unknown.

#include <string>

#include <grpcpp/support/status.h>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "ai/pipestream/vlm/v1/vlm_convert.pb.h"

namespace grparse {

// passes and triple_regions against their *_raw strings, and
// crop_batch_size of at least 1.
grpc::Status validate_nemotron_parse(
    const ai::pipestream::parse::v1::NemotronParseOptions& options, const std::string& path,
    const std::string& surface);

// A checked NemotronParseOptions as the VLM convert collector's own
// message: the typed value where it is set, else the member the raw string
// names. Omitted fields stay omitted, so the collector's docling defaults
// apply.
ai::pipestream::vlm::v1::NemotronParseOptions collector_nemotron_parse(
    const ai::pipestream::parse::v1::NemotronParseOptions& options);

// Each engine override's torch_dtype_enum against its torch_dtype string.
grpc::Status validate_engine_overrides(const ai::pipestream::parse::v1::VlmModelSpec& spec,
                                       const std::string& path, const std::string& surface);

}  // namespace grparse
