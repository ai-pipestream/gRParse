#include "grparse/service_version.h"

// The only translation unit that sees GRPARSE_SOURCE_DIGEST: CMake sets it
// on this file alone (set_source_files_properties in CMakeLists.txt), so a
// commit recompiles this one object instead of every object that includes
// service_version.h, and ccache keeps the rest (#239).
#ifndef GRPARSE_SOURCE_DIGEST
#define GRPARSE_SOURCE_DIGEST "unknown"
#endif

namespace grparse {

std::string_view source_digest() noexcept { return GRPARSE_SOURCE_DIGEST; }

}  // namespace grparse
