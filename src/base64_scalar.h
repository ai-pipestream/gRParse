// The scalar base64 reference implementations. The public entry points in
// base64.cpp use vector kernels for the bulk work and delegate to these to
// own the accept/reject contract: any fast-path error re-runs here so the
// decision and the exception are always the reference ones. The test suite
// includes this header directly to fuzz the two paths against each other.
#ifndef GRPARSE_SRC_BASE64_SCALAR_H
#define GRPARSE_SRC_BASE64_SCALAR_H

#include <cstddef>
#include <string>

namespace grparse::detail {

// Throws std::invalid_argument on any input the wire contract rejects:
// empty input, a stripped length not divisible by four, characters outside
// the standard alphabet, or padding anywhere but a final complete quad.
// ASCII whitespace is ignored wherever it appears.
std::string scalar_decode_base64(const std::string& value);

std::string scalar_encode_base64(const void* data, std::size_t size);

}  // namespace grparse::detail

#endif
