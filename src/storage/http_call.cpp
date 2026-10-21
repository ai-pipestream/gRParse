#include "http_call.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <memory>
#include <mutex>
#include <stdexcept>

#include <arpa/inet.h>

#include <curl/curl.h>

namespace grparse::storage {
namespace {

void ensure_curl_initialized() {
  static std::once_flag once;
  static CURLcode result = CURLE_OK;
  std::call_once(once, [] { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
  if (result != CURLE_OK) throw std::runtime_error("storage connector could not initialize libcurl");
}

struct Cursor {
  const std::string* bytes;
  size_t offset = 0;
};

size_t read_body(char* buffer, size_t size, size_t count, void* context) {
  auto* cursor = static_cast<Cursor*>(context);
  const size_t available = std::min(size * count, cursor->bytes->size() - cursor->offset);
  std::memcpy(buffer, cursor->bytes->data() + cursor->offset, available);
  cursor->offset += available;
  return available;
}

struct BodySink {
  std::string* body;
  uint64_t cap = 0;
  bool overflowed = false;
};

size_t write_body(char* buffer, size_t size, size_t count, void* context) {
  auto* sink = static_cast<BodySink*>(context);
  if (sink->cap != 0 && sink->body->size() + size * count > sink->cap) {
    sink->overflowed = true;
    return 0;
  }
  sink->body->append(buffer, size * count);
  return size * count;
}

size_t write_header(char* buffer, size_t size, size_t count, void* context) {
  const size_t length = size * count;
  std::string_view line(buffer, length);
  // A new status line starts a new response (after an interim 1xx): only
  // the final response's headers are kept.
  if (line.starts_with("HTTP/")) static_cast<HttpResponse*>(context)->headers.clear();
  const size_t colon = line.find(':');
  if (colon != std::string_view::npos) {
    std::string name(line.substr(0, colon));
    std::ranges::transform(name, name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.remove_suffix(1);
    static_cast<HttpResponse*>(context)->headers.emplace_back(name, std::string(value));
  }
  return length;
}

int check_peer(void* context, char* peer_ip, char*, int, int) {
  if (peer_ip != nullptr && public_address(peer_ip)) return CURL_PREREQFUNC_OK;
  *static_cast<bool*>(context) = true;
  return CURL_PREREQFUNC_ABORT;
}

bool public_ipv4(const unsigned char a[4]) {
  const uint32_t v = (uint32_t{a[0]} << 24) | (uint32_t{a[1]} << 16) | (uint32_t{a[2]} << 8) | a[3];
  struct Block {
    uint32_t base;
    int bits;
  };
  static constexpr Block kBlocked[] = {
      {0x00000000, 8},   // 0.0.0.0/8 "this network"
      {0x0A000000, 8},   // 10.0.0.0/8
      {0x64400000, 10},  // 100.64.0.0/10 shared address space (CGNAT)
      {0x7F000000, 8},   // 127.0.0.0/8 loopback
      {0xA9FE0000, 16},  // 169.254.0.0/16 link-local (cloud metadata)
      {0xAC100000, 12},  // 172.16.0.0/12
      {0xC0000000, 24},  // 192.0.0.0/24 IETF protocol assignments
      {0xC0000200, 24},  // 192.0.2.0/24 documentation
      {0xC0A80000, 16},  // 192.168.0.0/16
      {0xC6120000, 15},  // 198.18.0.0/15 benchmarking
      {0xC6336400, 24},  // 198.51.100.0/24 documentation
      {0xCB007100, 24},  // 203.0.113.0/24 documentation
      {0xE0000000, 4},   // 224.0.0.0/4 multicast
      {0xF0000000, 4},   // 240.0.0.0/4 reserved, broadcast included
  };
  for (const Block& block : kBlocked) {
    const uint32_t mask = ~uint32_t{0} << (32 - block.bits);
    if ((v & mask) == block.base) return false;
  }
  return true;
}

}  // namespace

void guard_public_peers(void* curl_handle, bool* refused) {
  if (private_endpoints_allowed()) return;
  curl_easy_setopt(static_cast<CURL*>(curl_handle), CURLOPT_PREREQFUNCTION, check_peer);
  curl_easy_setopt(static_cast<CURL*>(curl_handle), CURLOPT_PREREQDATA, refused);
}

bool private_endpoints_allowed() {
  const char* value = std::getenv("GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS");
  return value != nullptr && std::string_view(value) == "1";
}

uint64_t source_max_bytes() {
  constexpr uint64_t kDefault = 256ull * 1024 * 1024;
  const char* value = std::getenv("GRPARSE_SOURCE_MAX_BYTES");
  if (value == nullptr || *value == '\0') return kDefault;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0 || value[0] == '-') {
    throw std::invalid_argument("GRPARSE_SOURCE_MAX_BYTES must be a positive byte count");
  }
  return parsed;
}

bool public_address(const std::string& ip) {
  unsigned char v4[4];
  if (inet_pton(AF_INET, ip.c_str(), v4) == 1) return public_ipv4(v4);
  unsigned char v6[16];
  if (inet_pton(AF_INET6, ip.c_str(), v6) != 1) return false;
  static constexpr unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
  static constexpr unsigned char kNat64[12] = {0x00, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0};
  if (std::memcmp(v6, kMapped, 12) == 0 || std::memcmp(v6, kNat64, 12) == 0) {
    return public_ipv4(v6 + 12);
  }
  // ::/96 covers :: and ::1 and the deprecated IPv4-compatible form.
  static constexpr unsigned char kZero[12] = {};
  if (std::memcmp(v6, kZero, 12) == 0) return false;
  if ((v6[0] & 0xFE) == 0xFC) return false;                   // fc00::/7 unique local
  if (v6[0] == 0xFE && (v6[1] & 0xC0) == 0x80) return false;  // fe80::/10 link-local
  if (v6[0] == 0xFE && (v6[1] & 0xC0) == 0xC0) return false;  // fec0::/10 site-local
  if (v6[0] == 0xFF) return false;                            // ff00::/8 multicast
  if (v6[0] == 0x20 && v6[1] == 0x01 && v6[2] == 0x0D && v6[3] == 0xB8) return false;  // 2001:db8::/32
  return true;
}

std::string endpoint_problem(const std::string& url) {
  if (!url.starts_with("https://") && !url.starts_with("http://")) return "must be an http or https URL";
  if (std::ranges::any_of(url, [](unsigned char c) { return c <= 0x20 || c == 0x7F; })) {
    return "carries whitespace or a control character";
  }
  if (url.find_first_of("?#") != std::string::npos) return "carries a query or fragment";
  const size_t authority = url.find("://") + 3;
  const size_t authority_end = url.find('/', authority);
  if (url.substr(authority, authority_end == std::string::npos ? std::string::npos
                                                               : authority_end - authority)
          .find('@') != std::string::npos) {
    return "carries userinfo";
  }
  if (url_origin(url).empty()) return "names no host";
  return std::string();
}

std::string url_origin(const std::string& url) {
  const std::unique_ptr<CURLU, void (*)(CURLU*)> parsed(curl_url(), curl_url_cleanup);
  if (parsed == nullptr || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) {
    return std::string();
  }
  const auto part = [&](CURLUPart which, unsigned flags) {
    char* value = nullptr;
    if (curl_url_get(parsed.get(), which, &value, flags) != CURLUE_OK || value == nullptr) {
      return std::string();
    }
    std::string out(value);
    curl_free(value);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
  };
  const std::string scheme = part(CURLUPART_SCHEME, 0);
  const std::string host = part(CURLUPART_HOST, 0);
  const std::string port = part(CURLUPART_PORT, CURLU_DEFAULT_PORT);
  if (scheme.empty() || host.empty()) return std::string();
  return scheme + "://" + host + ":" + port;
}

std::string HttpResponse::header(const std::string& name) const {
  for (const auto& [key, value] : headers) {
    if (key == name) return value;
  }
  return std::string();
}

HttpResponse http_call(const HttpRequest& request, const std::string& what) {
  ensure_curl_initialized();
  const std::unique_ptr<CURL, void (*)(CURL*)> handle(curl_easy_init(), curl_easy_cleanup);
  if (handle == nullptr) throw std::runtime_error(what + ": could not create a transfer");
  curl_slist* raw = nullptr;
  for (const auto& [name, value] : request.headers) {
    curl_slist* next = curl_slist_append(raw, (name + ": " + value).c_str());
    if (next == nullptr) {
      curl_slist_free_all(raw);
      throw std::runtime_error(what + ": could not build its headers");
    }
    raw = next;
  }
  if (curl_slist* next = curl_slist_append(raw, "Expect:"); next != nullptr) raw = next;
  const std::unique_ptr<curl_slist, void (*)(curl_slist*)> headers(raw, curl_slist_free_all);

  HttpResponse response;
  Cursor cursor{&request.body, 0};
  CURL* curl = handle.get();
  curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
  BodySink sink{&response.body, request.max_body_bytes, false};
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  bool peer_refused = false;
  if (request.public_peers_only) guard_public_peers(curl, &peer_refused);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.timeout_seconds);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (request.method == "PUT") {
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_body);
    curl_easy_setopt(curl, CURLOPT_READDATA, &cursor);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
  } else if (request.method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(request.body.size()));
  } else if (request.method != "GET") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
  }
  if (!request.verify_ssl) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  const CURLcode transfer = curl_easy_perform(curl);
  if (peer_refused) {
    throw HttpPeerRefused(what + ": the endpoint is a loopback, private, link-local or "
                          "otherwise non-public address (GRPARSE_STORAGE_ALLOW_PRIVATE_ENDPOINTS=1 "
                          "allows it)");
  }
  if (sink.overflowed) {
    throw std::runtime_error(what + ": the body is larger than " +
                             std::to_string(request.max_body_bytes) + " bytes");
  }
  if (transfer != CURLE_OK) {
    throw std::runtime_error(what + ": " + curl_easy_strerror(transfer));
  }
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
  const char* location = nullptr;
  curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
  if (location != nullptr) response.redirect_url = location;
  return response;
}

std::string percent_encode(const std::string& text, bool keep_slash) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : text) {
    if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~' ||
        (keep_slash && c == '/')) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    }
  }
  return out;
}

}  // namespace grparse::storage
