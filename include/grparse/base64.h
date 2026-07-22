#pragma once

#include <cstddef>
#include <string>

namespace grparse {

// Decode standard base64 (with optional whitespace). Throws std::invalid_argument on bad input.
std::string decode_base64(const std::string& value);

// Encode bytes as standard padded base64 (no line breaks).
std::string encode_base64(const void* data, size_t size);

}  // namespace grparse
