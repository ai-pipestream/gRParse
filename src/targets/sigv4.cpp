#include "sigv4.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <stdexcept>

#include "sha256.h"

namespace grparse::targets {
namespace {

constexpr std::string_view kAlgorithm = "AWS4-HMAC-SHA256";
constexpr std::string_view kTerminator = "aws4_request";

// The specification's header normalization: outer whitespace trimmed, runs of
// inner whitespace collapsed to one space.
std::string normalize_value(std::string_view value) {
  std::string normalized;
  normalized.reserve(value.size());
  bool pending_space = false;
  for (const char character : value) {
    if (static_cast<unsigned char>(character) == ' ' || character == '\t') {
      pending_space = !normalized.empty();
      continue;
    }
    if (pending_space) normalized.push_back(' ');
    pending_space = false;
    normalized.push_back(character);
  }
  return normalized;
}

// The signed headers, sorted by name.  Sorting here rather than at the call
// site means the caller cannot get the order wrong.
std::vector<std::pair<std::string, std::string>> sorted_headers(const SigV4Request& request) {
  std::vector<std::pair<std::string, std::string>> headers = request.headers;
  for (auto& [name, value] : headers) {
    std::ranges::transform(name, name.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    value = normalize_value(value);
  }
  std::ranges::sort(headers, {}, &std::pair<std::string, std::string>::first);
  return headers;
}

std::string signed_header_list(
    const std::vector<std::pair<std::string, std::string>>& headers) {
  std::string list;
  for (const auto& header : headers) {
    if (!list.empty()) list.push_back(';');
    list += header.first;
  }
  return list;
}

// The date half of the ISO 8601 basic stamp, which is what the credential
// scope and the first key derivation step are keyed on.
std::string_view scope_date(const SigV4Request& request) {
  if (request.amz_date.size() < 8) {
    throw std::runtime_error("signature v4 needs an ISO 8601 basic timestamp");
  }
  return std::string_view(request.amz_date).substr(0, 8);
}

}  // namespace

std::string uri_encode(std::string_view value, bool path) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                            byte == '.' || byte == '~';
    if (unreserved || (path && byte == '/')) {
      encoded.push_back(character);
      continue;
    }
    encoded.push_back('%');
    encoded.push_back(kDigits[byte >> 4]);
    encoded.push_back(kDigits[byte & 0x0f]);
  }
  return encoded;
}

std::string amz_timestamp(int64_t unix_seconds) {
  const auto seconds = static_cast<std::time_t>(unix_seconds);
  std::tm utc{};
  if (gmtime_r(&seconds, &utc) == nullptr) {
    throw std::runtime_error("signature v4 could not resolve the current time");
  }
  std::array<char, 32> stamp{};
  const size_t written = std::strftime(stamp.data(), stamp.size(), "%Y%m%dT%H%M%SZ", &utc);
  if (written == 0) throw std::runtime_error("signature v4 could not format the timestamp");
  return std::string(stamp.data(), written);
}

std::string canonical_request(const SigV4Request& request) {
  const auto headers = sorted_headers(request);
  std::string canonical = request.method;
  canonical += '\n';
  canonical += request.canonical_uri;
  canonical += '\n';
  canonical += request.canonical_query;
  canonical += '\n';
  for (const auto& [name, value] : headers) {
    canonical += name;
    canonical += ':';
    canonical += value;
    canonical += '\n';
  }
  canonical += '\n';
  canonical += signed_header_list(headers);
  canonical += '\n';
  canonical += request.payload_sha256_hex;
  return canonical;
}

std::string credential_scope(const SigV4Request& request) {
  std::string scope(scope_date(request));
  scope += '/';
  scope += request.region;
  scope += '/';
  scope += request.service;
  scope += '/';
  scope += kTerminator;
  return scope;
}

std::string string_to_sign(const SigV4Request& request) {
  std::string sign(kAlgorithm);
  sign += '\n';
  sign += request.amz_date;
  sign += '\n';
  sign += credential_scope(request);
  sign += '\n';
  sign += sha256_hex(canonical_request(request));
  return sign;
}

std::string signing_key(std::string_view secret_key, std::string_view date,
                        std::string_view region, std::string_view service) {
  std::string key = "AWS4";
  key += secret_key;
  const std::string dated = hmac_sha256(key, date);
  const std::string regioned = hmac_sha256(dated, region);
  const std::string serviced = hmac_sha256(regioned, service);
  return hmac_sha256(serviced, kTerminator);
}

std::string authorization_header(std::string_view access_key, std::string_view secret_key,
                                 const SigV4Request& request) {
  const auto headers = sorted_headers(request);
  const std::string key =
      signing_key(secret_key, scope_date(request), request.region, request.service);
  const std::string signature = to_hex(hmac_sha256(key, string_to_sign(request)));
  std::string authorization(kAlgorithm);
  authorization += " Credential=";
  authorization += access_key;
  authorization += '/';
  authorization += credential_scope(request);
  authorization += ", SignedHeaders=";
  authorization += signed_header_list(headers);
  authorization += ", Signature=";
  authorization += signature;
  return authorization;
}

}  // namespace grparse::targets
