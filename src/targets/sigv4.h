// AWS Signature Version 4 for the S3 target.  Only what an object PUT needs:
// header-signed requests with an already-hashed payload, no query signing, no
// chunked upload signing, no session tokens.  The signer is pure so it can be
// held to the published test vectors; nothing here opens a socket.
//
// Secrets never leave this boundary.  The functions take the secret key,
// return a signature, and put neither into any message: an error raised
// anywhere above must be able to name what failed without naming who signed
// it.
#ifndef GRPARSE_TARGETS_SIGV4_H
#define GRPARSE_TARGETS_SIGV4_H

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grparse::targets {

// One request to sign.  `headers` are the headers that take part in the
// signature, names already lowercased; the signer sorts them and derives the
// SignedHeaders list from them, so a caller that omits one from the wire has
// produced an invalid request rather than a differently signed one.
struct SigV4Request {
  std::string method;
  // The path, already URI-encoded as it appears on the wire.
  std::string canonical_uri;
  // The canonical query string (sorted encoded pairs), empty for a bare path.
  std::string canonical_query;
  std::vector<std::pair<std::string, std::string>> headers;
  // Lowercase hex SHA-256 of the request body.
  std::string payload_sha256_hex;
  std::string region;
  std::string service;
  // ISO 8601 basic UTC, "YYYYMMDDTHHMMSSZ".
  std::string amz_date;
};

// The canonical request, verbatim as the specification composes it.
std::string canonical_request(const SigV4Request& request);

// The credential scope, "YYYYMMDD/region/service/aws4_request".
std::string credential_scope(const SigV4Request& request);

// The string to sign, over the canonical request's digest.
std::string string_to_sign(const SigV4Request& request);

// The date-, region-, and service-scoped signing key, as raw bytes.
std::string signing_key(std::string_view secret_key, std::string_view date,
                        std::string_view region, std::string_view service);

// The complete Authorization header value.
std::string authorization_header(std::string_view access_key, std::string_view secret_key,
                                 const SigV4Request& request);

// RFC 3986 unreserved-character encoding, with "/" left alone when `path` is
// set: the form the canonical URI of an object key takes.
std::string uri_encode(std::string_view value, bool path);

// The ISO 8601 basic UTC stamp of `unix_seconds`, which is what the
// x-amz-date header and the credential scope are cut from.
std::string amz_timestamp(int64_t unix_seconds);

}  // namespace grparse::targets

#endif
