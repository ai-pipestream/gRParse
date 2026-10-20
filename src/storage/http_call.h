// One HTTP exchange over libcurl for the storage connectors (Azure Blob,
// Google Cloud Storage, Google Drive, S3). The response body is returned to the
// caller and never logged: a store's error document can quote a credential
// back, so errors name the operation and the HTTP status only.
#ifndef GRPARSE_STORAGE_HTTP_CALL_H
#define GRPARSE_STORAGE_HTTP_CALL_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grparse::storage {

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool verify_ssl = true;
  long timeout_seconds = 300;
  // A response body larger than this fails the exchange. The default bounds
  // the listings, metadata and token replies; a document download sets
  // source_max_bytes().
  uint64_t max_body_bytes = kDefaultMaxResponseBytes;
  // Refuse to send the request when the connected peer's address is not a
  // public unicast address (loopback, private, link-local, CGNAT, multicast,
  // reserved), unless private_endpoints_allowed(). Checked on the socket
  // curl actually dialled, so a hostname that resolves to an internal
  // address is refused too. Redirects are never followed, so each hop is a
  // call of its own and checked again.
  bool public_peers_only = true;

  static constexpr uint64_t kDefaultMaxResponseBytes = 16ull * 1024 * 1024;
};

struct HttpResponse {
  long status = 0;
  std::string body;
  // Response headers, names lowercased.
  std::vector<std::pair<std::string, std::string>> headers;
  // For a 3xx with a Location header: that location resolved against the
  // request URL. Never followed by http_call.
  std::string redirect_url;

  bool ok() const { return status >= 200 && status < 300; }
  // The first header of that (lowercase) name, or empty.
  std::string header(const std::string& name) const;
};

// Thrown when public_peers_only refused the peer the request would have
// gone to. The message names no address.
class HttpPeerRefused : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Performs the exchange. Throws std::runtime_error naming `what` on a
// transport failure or a body past max_body_bytes, HttpPeerRefused when
// public_peers_only refused the peer; an HTTP error status (a redirect
// included) is returned, not thrown.
HttpResponse http_call(const HttpRequest& request, const std::string& what);

// GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS=1, read per call: the deployment
// lets callers name loopback and private-network endpoints (emulators, an
// in-cluster store) and plain-http token endpoints. Unset, every endpoint a
// request names must be a public address, and token endpoints https.
bool private_endpoints_allowed();

// GRPARSE_SOURCE_MAX_BYTES (bytes; default 268435456, 256 MiB): the largest
// document a remote source downloads. std::invalid_argument when set to
// anything but a positive integer.
uint64_t source_max_bytes();

// Arms a libcurl easy handle (passed as void* so this header stays free of
// curl.h) with the public-peer check http_call applies, unless
// private_endpoints_allowed(). `*refused` is set when the check aborted the
// transfer. For a client that drives its own handle (the S3 upload).
void guard_public_peers(void* curl_handle, bool* refused);

// Whether `ip` (a numeric IPv4 or IPv6 address) is a public unicast address.
// False for anything unparsable. IPv4-mapped and NAT64 IPv6 addresses are
// judged by the IPv4 address they carry.
bool public_address(const std::string& ip);

// Why `url` cannot serve as a base endpoint that request paths are appended
// to, or empty when it can: not http or https, a whitespace or control
// character, userinfo, a query or a fragment (which would swallow the path
// appended after it).
std::string endpoint_problem(const std::string& url);

// "scheme://host:port" of an absolute URL, lowercased, with the scheme's
// default port filled in; empty when the URL does not parse.
std::string url_origin(const std::string& url);

// RFC 3986 percent-encoding of everything outside the unreserved set; with
// `keep_slash` a '/' stays as is.
std::string percent_encode(const std::string& text, bool keep_slash);

}  // namespace grparse::storage

#endif
