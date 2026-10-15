#include "http_call.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>

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

size_t write_body(char* buffer, size_t size, size_t count, void* context) {
  static_cast<std::string*>(context)->append(buffer, size * count);
  return size * count;
}

size_t write_header(char* buffer, size_t size, size_t count, void* context) {
  const size_t length = size * count;
  std::string_view line(buffer, length);
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

}  // namespace

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
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
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
  if (transfer != CURLE_OK) {
    throw std::runtime_error(what + ": " + curl_easy_strerror(transfer));
  }
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
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
