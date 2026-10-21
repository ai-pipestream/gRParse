// OAuth 2.0 for the Google connectors: a service-account JWT assertion
// (RFC 7523, RS256) for Cloud Storage, and a refresh-token grant
// (RFC 6749 section 6) for Drive. Keys and secrets are used to sign or are
// sent to the token endpoint and are never put into a message.
#ifndef GRPARSE_STORAGE_GOOGLE_AUTH_H
#define GRPARSE_STORAGE_GOOGLE_AUTH_H

#include <cstdint>
#include <string>

#include "ai/pipestream/parse/v1/parse_types.pb.h"

namespace grparse::storage {

// base64url without padding (RFC 4648 section 5).
std::string base64url(const std::string& bytes);

// RSASSA-PKCS1-v1_5 SHA-256 signature of `data` with a PEM private key.
// std::invalid_argument when the key does not parse.
std::string rs256_sign(const std::string& private_key_pem, const std::string& data);

// The signed JWT assertion for `key` and `scope`, issued at `now`.
std::string service_account_assertion(
    const ai::pipestream::parse::v1::GoogleCloudStorageServiceAccountKey& key,
    const std::string& scope, uint32_t now);

// Exchanges the assertion at key.token_uri for an access token.
std::string service_account_access_token(
    const ai::pipestream::parse::v1::GoogleCloudStorageServiceAccountKey& key,
    const std::string& scope);

// Exchanges a refresh token at credentials.token_uri for an access token.
std::string refresh_access_token(const ai::pipestream::parse::v1::GoogleDriveCredentials& credentials,
                                 const std::string& refresh_token);

}  // namespace grparse::storage

#endif
