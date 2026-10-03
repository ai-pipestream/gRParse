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

}  // namespace grparse
