// A differential target for the base64 codec: the simdutf fast paths that
// every upload and every page image go through (decode_base64,
// encode_base64) against the scalar reference implementations that own the
// wire contract (detail::scalar_decode_base64, scalar_encode_base64).
//
// The contract under test, on every input:
//   - decode: both accept with identical bytes, or both reject with
//     std::invalid_argument;
//   - encode: identical output, which decodes back to the input through
//     both decoders, also with whitespace spliced in and with the padding
//     cut (which both must reject when it leaves a partial quad).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "base64_scalar.h"
#include "grparse/base64.h"

namespace {

std::optional<std::string> fast_decode(const std::string& text) {
  try {
    return grparse::decode_base64(text);
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
}

std::optional<std::string> scalar_decode(const std::string& text) {
  try {
    return grparse::detail::scalar_decode_base64(text);
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
}

void require_same_decode(const std::string& text) {
  const std::optional<std::string> fast = fast_decode(text);
  const std::optional<std::string> scalar = scalar_decode(text);
  if (fast.has_value() != scalar.has_value()) __builtin_trap();  // accept/reject diverged
  if (fast.has_value() && *fast != *scalar) __builtin_trap();    // decoded bytes diverged
}

// Whitespace the contract ignores, spliced in at positions the input picks.
std::string with_whitespace(const std::string& encoded, const uint8_t* data, size_t size) {
  static constexpr char kSpace[] = {' ', '\n', '\r', '\t', '\v', '\f'};
  std::string out;
  out.reserve(encoded.size() * 2);
  for (size_t i = 0; i < encoded.size(); ++i) {
    const uint8_t pick = size == 0 ? 0 : data[i % size];
    if ((pick & 0x07) == 0) out.push_back(kSpace[(pick >> 3) % sizeof(kSpace)]);
    out.push_back(encoded[i]);
  }
  if (size > 0 && (data[0] & 0x80) != 0) out.push_back('\n');
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // The input as base64 text: the decoders must agree on arbitrary bytes.
  const std::string text(reinterpret_cast<const char*>(data), size);
  require_same_decode(text);

  // The input as binary: the encoders must agree, and round-trip.
  const std::string fast = grparse::encode_base64(data, size);
  const std::string scalar = grparse::detail::scalar_encode_base64(data, size);
  if (fast != scalar) __builtin_trap();
  if (size > 0) {
    const std::optional<std::string> back = fast_decode(fast);
    if (!back.has_value() || *back != text) __builtin_trap();
    require_same_decode(fast);
    require_same_decode(with_whitespace(fast, data, size));
    // Padding cut off: a partial final quad, which the contract rejects.
    if (fast.back() == '=') {
      std::string unpadded = fast;
      while (!unpadded.empty() && unpadded.back() == '=') unpadded.pop_back();
      require_same_decode(unpadded);
      if (fast_decode(unpadded).has_value()) __builtin_trap();
    }
  }
  return 0;
}
