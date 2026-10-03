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
// SHA-256 of the source tree, computed when CMake configures
// (CMakeLists.txt, GRPARSE_SOURCE_DIGEST): it tells apart two builds that
// carry the same unbumped version.
#ifndef GRPARSE_SOURCE_DIGEST
#define GRPARSE_SOURCE_DIGEST "unknown"
#endif

namespace grparse {

inline constexpr std::string_view kServiceVersion =
    "grparse-" GRPARSE_VERSION "-" GRPARSE_ORT_PACKAGE_NAME;

inline constexpr std::string_view kSourceDigest = GRPARSE_SOURCE_DIGEST;

}  // namespace grparse
