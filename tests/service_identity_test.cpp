#include <string>
#include <string_view>

#include "grparse/service_version.h"
#include "support/check.h"

// The digest CMake computed at configure time, handed to this one test TU
// under a different name so GRPARSE_SOURCE_DIGEST itself stays on exactly
// one command line (src/service_identity.cpp, #239).
#ifndef GRPARSE_STAMPED_DIGEST
#error "CMakeLists.txt sets GRPARSE_STAMPED_DIGEST on tests/service_identity_test.cpp"
#endif

namespace {

using grparse_test::require;
using grparse_test::require_equal;

bool lowercase_hex(std::string_view value) {
  for (const char c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

}  // namespace

int main() {
  return grparse_test::run_test_main("service-identity-test", {
      [] {
        const std::string_view digest = grparse::source_digest();
        require_equal(digest.size(), std::size_t{64},
                      "source_digest() is a SHA-256 in hex, never empty or 'unknown'");
        require(lowercase_hex(digest), "source_digest() is lowercase hex: " + std::string(digest));
        require_equal(digest, std::string_view(GRPARSE_STAMPED_DIGEST),
                      "source_digest() serves the digest CMake stamped at configure time");
      },
      [] {
        require(grparse::kServiceVersion.starts_with("grparse-"),
                "the served version keeps its grparse-<version>-<flavor> shape");
      },
  });
}
