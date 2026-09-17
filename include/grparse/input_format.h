#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "ai/pipestream/parse/v1/parse_types.pb.h"

namespace grparse {

// Maps a resolved origin mimetype (and filename as a tie-breaker for
// ambiguous types like application/xml) onto the ConvertDocumentOptions
// InputFormat allowlist vocabulary. nullopt means the type is outside the
// InputFormat enum (or unknown), so a non-empty from_formats list rejects it.
std::optional<ai::pipestream::parse::v1::InputFormat> input_format_for(
    std::string_view mimetype, const std::filesystem::path& filename);

// True when from_formats is empty (Docling default: all formats) or when
// detected is one of the listed tags.
bool from_formats_allows(
    const google::protobuf::RepeatedField<int>& from_formats,
    ai::pipestream::parse::v1::InputFormat detected);

}  // namespace grparse
