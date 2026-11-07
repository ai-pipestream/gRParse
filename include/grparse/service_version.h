#pragma once

#include <string_view>

// The version string this build serves and stamps on what it produces:
// "grparse-<version>-<flavor>". Both halves come from the build: the version
// from the project() line in CMakeLists.txt, so a release bumps one place,
// and the flavor from the ONNX Runtime package, so the OpenVINO and CPU
// images stop claiming cuda.
#ifndef GRPARSE_VERSION
#define GRPARSE_VERSION "unversioned"
#endif
#ifndef GRPARSE_ORT_PACKAGE_NAME
#define GRPARSE_ORT_PACKAGE_NAME "unknown"
#endif

namespace grparse {

inline constexpr std::string_view kServiceVersion =
    "grparse-" GRPARSE_VERSION "-" GRPARSE_ORT_PACKAGE_NAME;

// SHA-256 of the source tree, computed when CMake configures
// (cmake/SourceDigest.cmake): it tells apart two builds that carry the same
// unbumped version. A function, not a constant: the digest changes on every
// commit, so only src/service_identity.cpp is compiled with it (#239).
std::string_view source_digest() noexcept;

}  // namespace grparse
