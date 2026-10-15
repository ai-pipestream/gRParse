// One HTTP exchange over libcurl for the storage connectors (Azure Blob,
// Google Cloud Storage, Google Drive). The response body is returned to the
// caller and never logged: a store's error document can quote a credential
// back, so errors name the operation and the HTTP status only.
#ifndef GRPARSE_STORAGE_HTTP_CALL_H
#define GRPARSE_STORAGE_HTTP_CALL_H

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
};

struct HttpResponse {
  long status = 0;
  std::string body;
  // Response headers, names lowercased.
  std::vector<std::pair<std::string, std::string>> headers;

  bool ok() const { return status >= 200 && status < 300; }
  // The first header of that (lowercase) name, or empty.
  std::string header(const std::string& name) const;
};

// Performs the exchange. Throws std::runtime_error naming `what` on a
// transport failure; an HTTP error status is returned, not thrown.
HttpResponse http_call(const HttpRequest& request, const std::string& what);

// RFC 3986 percent-encoding of everything outside the unreserved set; with
// `keep_slash` a '/' stays as is.
std::string percent_encode(const std::string& text, bool keep_slash);

}  // namespace grparse::storage

#endif
