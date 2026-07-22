#pragma once

#include <string>

namespace grparse {

// Decode standard base64 (with optional whitespace). Throws std::invalid_argument on bad input.
std::string decode_base64(const std::string& value);

}  // namespace grparse
