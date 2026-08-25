#include "s3_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include "sha256.h"
#include "sigv4.h"

namespace grparse::targets {
namespace {

constexpr std::string_view kService = "s3";
constexpr std::string_view kDefaultRegion = "us-east-1";
constexpr std::string_view kContentType = "application/octet-stream";
constexpr long kConnectTimeoutSeconds = 10;
constexpr long kTransferTimeoutSeconds = 300;

// libcurl's global initializer is not thread safe and must run before any
// handle exists.  Every put_object goes through here first, so the uploads
// fanned out across the executor cannot race it.
void ensure_curl_initialized() {
  static std::once_flag once;
  static CURLcode result = CURLE_OK;
  std::call_once(once, [] { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
  if (result != CURLE_OK) throw std::runtime_error("s3 target could not initialize libcurl");
}

// The upload body, read straight out of the caller's string.
struct BodyCursor {
  std::string_view bytes;
  size_t offset = 0;
};

size_t read_body(char* buffer, size_t size, size_t count, void* context) {
  auto* cursor = static_cast<BodyCursor*>(context);
  const size_t wanted = size * count;
  const size_t available = std::min(wanted, cursor->bytes.size() - cursor->offset);
  std::memcpy(buffer, cursor->bytes.data() + cursor->offset, available);
  cursor->offset += available;
  return available;
}

// The response body is read and dropped.  Not writing it anywhere is the
// point: libcurl's default sink is stdout, and an S3 error document can
// contain the access key that failed.
size_t discard_body(char*, size_t size, size_t count, void*) { return size * count; }

size_t collect_etag(char* buffer, size_t size, size_t count, void* context) {
  const size_t length = size * count;
  const std::string_view line(buffer, length);
  constexpr std::string_view kEtag = "etag:";
  if (line.size() > kEtag.size()) {
    std::string name(line.substr(0, kEtag.size()));
    std::ranges::transform(name, name.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    if (name == kEtag) {
      std::string_view value = line.substr(kEtag.size());
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
      }
      while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
      }
      *static_cast<std::string*>(context) = std::string(value);
    }
  }
  return length;
}

// Splits "https://host:port/path" into its parts, defaulting the scheme to
// https the way every S3 endpoint field is written without one.
void split_endpoint(const std::string& endpoint, std::string* scheme, std::string* authority,
                    std::string* path) {
  std::string_view rest(endpoint);
  *scheme = "https";
  if (rest.starts_with("http://")) {
    *scheme = "http";
    rest.remove_prefix(std::string_view("http://").size());
  } else if (rest.starts_with("https://")) {
    rest.remove_prefix(std::string_view("https://").size());
  }
  const size_t slash = rest.find('/');
  if (slash == std::string_view::npos) {
    *authority = std::string(rest);
    path->clear();
  } else {
    *authority = std::string(rest.substr(0, slash));
    *path = std::string(rest.substr(slash));
    while (!path->empty() && path->back() == '/') path->pop_back();
  }
}

// The Host header libcurl will actually send: it drops the port when it is
// the scheme's default, and a signature over a header the request does not
// carry is a signature the store rejects.
std::string host_header_for(const std::string& scheme, const std::string& authority) {
  const size_t colon = authority.rfind(':');
  if (colon == std::string::npos) return authority;
  const std::string_view port(authority.data() + colon + 1, authority.size() - colon - 1);
  if ((scheme == "http" && port == "80") || (scheme == "https" && port == "443")) {
    return authority.substr(0, colon);
  }
  return authority;
}

}  // namespace

std::string region_for_endpoint(const std::string& endpoint) {
  std::string scheme;
  std::string authority;
  std::string path;
  split_endpoint(endpoint, &scheme, &authority, &path);
  const size_t colon = authority.find(':');
  const std::string host = colon == std::string::npos ? authority : authority.substr(0, colon);

  // "s3.<region>.amazonaws.com" and "<bucket>.s3.<region>.amazonaws.com" both
  // name the region in the label after "s3".  Anything else is a store that
  // does not encode one, and us-east-1 is what those accept.
  std::vector<std::string_view> labels;
  std::string_view rest(host);
  while (!rest.empty()) {
    const size_t dot = rest.find('.');
    if (dot == std::string_view::npos) {
      labels.push_back(rest);
      break;
    }
    labels.push_back(rest.substr(0, dot));
    rest.remove_prefix(dot + 1);
  }
  for (size_t index = 0; index + 1 < labels.size(); ++index) {
    if (labels[index] != "s3" && !labels[index].starts_with("s3-")) continue;
    const std::string_view candidate = labels[index + 1];
    // A region label is lowercase letters, digits, and hyphens, and is never
    // the registry suffix that follows a regionless "s3.amazonaws.com".
    if (candidate == "amazonaws" || candidate.empty()) break;
    const bool region_shaped = std::ranges::all_of(candidate, [](unsigned char character) {
      return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
             character == '-';
    });
    if (region_shaped) return std::string(candidate);
    break;
  }
  return std::string(kDefaultRegion);
}

S3Client::S3Client(S3Config config) : config_(std::move(config)) {
  if (config_.endpoint.empty()) {
    throw std::invalid_argument("S3Target requires an endpoint");
  }
  if (config_.bucket.empty()) throw std::invalid_argument("S3Target requires a bucket");
  if (config_.access_key.empty() || config_.secret_key.empty()) {
    throw std::invalid_argument("S3Target requires an access key and a secret key");
  }
  split_endpoint(config_.endpoint, &scheme_, &authority_, &base_path_);
  if (authority_.empty()) throw std::invalid_argument("S3Target endpoint names no host");
  host_header_ = host_header_for(scheme_, authority_);
  region_ = region_for_endpoint(config_.endpoint);
}

std::string S3Client::key_for(const std::string& path) const {
  if (config_.key_prefix.empty()) return path;
  std::string key = config_.key_prefix;
  while (!key.empty() && key.back() == '/') key.pop_back();
  key += '/';
  key += path;
  return key;
}

std::string S3Client::put_object(const std::string& key, const std::string& body) const {
  ensure_curl_initialized();

  const std::string canonical_uri =
      uri_encode(base_path_ + "/" + config_.bucket + "/" + key, true);
  const std::string payload_hash = sha256_hex(body);
  const std::string timestamp = amz_timestamp(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  SigV4Request signable;
  signable.method = "PUT";
  signable.canonical_uri = canonical_uri;
  signable.payload_sha256_hex = payload_hash;
  signable.region = region_;
  signable.service = std::string(kService);
  signable.amz_date = timestamp;
  signable.headers = {{"host", host_header_},
                      {"content-type", std::string(kContentType)},
                      {"x-amz-content-sha256", payload_hash},
                      {"x-amz-date", timestamp}};
  const std::string authorization =
      authorization_header(config_.access_key, config_.secret_key, signable);

  const std::unique_ptr<CURL, void (*)(CURL*)> handle(curl_easy_init(), curl_easy_cleanup);
  if (handle == nullptr) throw std::runtime_error("s3 target could not create a transfer");

  curl_slist* raw_headers = nullptr;
  const auto add_header = [&raw_headers](const std::string& header) {
    curl_slist* appended = curl_slist_append(raw_headers, header.c_str());
    if (appended == nullptr) throw std::runtime_error("s3 target could not build its headers");
    raw_headers = appended;
  };
  try {
    add_header("Content-Type: " + std::string(kContentType));
    add_header("x-amz-content-sha256: " + payload_hash);
    add_header("x-amz-date: " + timestamp);
    add_header("Authorization: " + authorization);
    // libcurl would otherwise negotiate a 100-continue on every body past a
    // kilobyte, which costs a round trip against stores that ignore it.
    add_header("Expect:");
  } catch (...) {
    curl_slist_free_all(raw_headers);
    throw;
  }
  const std::unique_ptr<curl_slist, void (*)(curl_slist*)> headers(raw_headers,
                                                                   curl_slist_free_all);

  const std::string url = scheme_ + "://" + authority_ + canonical_uri;
  BodyCursor cursor{body, 0};
  std::string etag;
  curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(handle.get(), CURLOPT_READFUNCTION, read_body);
  curl_easy_setopt(handle.get(), CURLOPT_READDATA, &cursor);
  curl_easy_setopt(handle.get(), CURLOPT_INFILESIZE_LARGE,
                   static_cast<curl_off_t>(body.size()));
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, discard_body);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, collect_etag);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &etag);
  curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
  curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, kTransferTimeoutSeconds);
  // Uploads run on pool threads; libcurl's signal-based resolver timeout is
  // not safe there.
  curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
  if (!config_.verify_ssl) {
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 0L);
  }

  const CURLcode transfer = curl_easy_perform(handle.get());
  if (transfer != CURLE_OK) {
    throw std::runtime_error("s3 target could not write '" + key + "': " +
                             curl_easy_strerror(transfer));
  }
  long status = 0;
  curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
  if (status < 200 || status >= 300) {
    throw std::runtime_error("s3 target could not write '" + key + "': the store answered HTTP " +
                             std::to_string(status));
  }
  return etag;
}

}  // namespace grparse::targets
