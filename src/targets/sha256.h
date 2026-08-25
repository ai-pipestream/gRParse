// SHA-256 and HMAC-SHA256 over byte strings.  The bundle manifest hashes
// every file with them and the Signature V4 signer derives its whole key
// chain from them, so they carry no dependency of their own: the server links
// gRPC's vendored BoringSSL statically, and reaching for a second crypto
// library in the same binary is how a process ends up with two incompatible
// definitions of the same symbol.
#ifndef GRPARSE_TARGETS_SHA256_H
#define GRPARSE_TARGETS_SHA256_H

#include <string>
#include <string_view>

namespace grparse::targets {

// The 32 raw digest bytes of `data`.
std::string sha256(std::string_view data);

// The digest as 64 lowercase hex characters, which is the form both the
// manifest and every Signature V4 field want.
std::string sha256_hex(std::string_view data);

// HMAC-SHA256 as 32 raw bytes.  Keys are secret material, so nothing here
// ever renders one: only the caller decides what becomes text.
std::string hmac_sha256(std::string_view key, std::string_view data);

// Lowercase hex of arbitrary bytes.
std::string to_hex(std::string_view bytes);

}  // namespace grparse::targets

#endif
