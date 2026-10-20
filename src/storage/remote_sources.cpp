#include "remote_sources.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "../source_parse.h"
#include "../targets/s3_client.h"
#include "../targets/target_step.h"
#include "http_call.h"
#include "xml_values.h"

namespace grparse::storage {
namespace {

namespace parsev1 = ai::pipestream::parse::v1;

constexpr int kMaxRedirects = 5;

std::string lowercase(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

class S3SourceStore final : public ObjectStore {
 public:
  explicit S3SourceStore(targets::S3Client client) : client_(std::move(client)) {}

  std::vector<std::string> list(const std::string& prefix, std::optional<uint32_t> limit) override {
    std::vector<std::string> names;
    std::string token;
    do {
      std::vector<std::pair<std::string, std::string>> query = {{"list-type", "2"},
                                                                 {"prefix", prefix}};
      if (limit.has_value()) query.emplace_back("max-keys", std::to_string(*limit));
      if (!token.empty()) query.emplace_back("continuation-token", token);
      const targets::S3Reply reply =
          client_.get("", query, HttpRequest::kDefaultMaxResponseBytes);
      if (reply.status < 200 || reply.status >= 300) {
        throw std::runtime_error("S3Source: listing the bucket answered HTTP " +
                                 std::to_string(reply.status));
      }
      for (std::string& key : xml_values(reply.body, "Key")) names.push_back(std::move(key));
      const auto truncated = xml_values(reply.body, "IsTruncated");
      const auto next = xml_values(reply.body, "NextContinuationToken");
      token = !truncated.empty() && truncated.front() == "true" && !next.empty() ? next.front()
                                                                                : std::string();
    } while (!token.empty() && (!limit.has_value() || names.size() < *limit));
    if (limit.has_value() && names.size() > *limit) names.resize(*limit);
    return names;
  }

  std::string get(const std::string& name) override {
    const targets::S3Reply reply = client_.get(name, {}, source_max_bytes());
    if (reply.status < 200 || reply.status >= 300) {
      throw std::runtime_error("S3Source: reading '" + name + "' answered HTTP " +
                               std::to_string(reply.status));
    }
    return reply.body;
  }

  WrittenObject put(const std::string&, const std::string&) override {
    throw std::logic_error("an S3 source is read-only");
  }

 private:
  targets::S3Client client_;
};

bool header_token_char(unsigned char c) {
  return std::isalnum(c) != 0 || std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) !=
                                     std::string_view::npos;
}

std::string percent_decode(std::string_view text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      out += static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

// A bare file name: no directory part, no control character, not "." or "..".
std::string bare_name(const std::string& name) {
  std::string base = name;
  const size_t slash = base.find_last_of("/\\");
  if (slash != std::string::npos) base = base.substr(slash + 1);
  for (const char c : base) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return std::string();
  }
  if (base == "." || base == "..") return std::string();
  return base;
}

std::optional<std::string> disposition_parameter(const std::string& header, const std::string& name) {
  const std::string lower = lowercase(header);
  size_t at = 0;
  while ((at = lower.find(name + "=", at)) != std::string::npos) {
    if (at != 0 && lower[at - 1] != ';' && lower[at - 1] != ' ') {
      at += name.size();
      continue;
    }
    size_t start = at + name.size() + 1;
    std::string value;
    if (start < header.size() && header[start] == '"') {
      const size_t end = header.find('"', start + 1);
      value = header.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1);
    } else {
      const size_t end = header.find(';', start);
      value = header.substr(start, end == std::string::npos ? std::string::npos : end - start);
      while (!value.empty() && value.back() == ' ') value.pop_back();
    }
    return value;
  }
  return std::nullopt;
}

}  // namespace

std::string http_source_filename(const std::string& url, const std::string& content_disposition) {
  if (!content_disposition.empty()) {
    if (const auto extended = disposition_parameter(content_disposition, "filename*")) {
      // RFC 5987: charset'language'percent-encoded
      const size_t quote = extended->find('\'', extended->find('\'') + 1);
      const std::string decoded =
          bare_name(percent_decode(quote == std::string::npos ? *extended : extended->substr(quote + 1)));
      if (!decoded.empty()) return decoded;
    }
    if (const auto plain = disposition_parameter(content_disposition, "filename")) {
      const std::string name = bare_name(*plain);
      if (!name.empty()) return name;
    }
  }
  std::string path = url;
  if (const size_t scheme = path.find("://"); scheme != std::string::npos) {
    const size_t slash = path.find('/', scheme + 3);
    path = slash == std::string::npos ? std::string() : path.substr(slash);
  }
  path = path.substr(0, path.find_first_of("?#"));
  const std::string name = bare_name(percent_decode(path));
  return name.empty() ? std::string("document") : name;
}

FetchedDocument fetch_s3_source(const parsev1::S3Source& source, const std::string& option_path) {
  targets::S3Config config;
  config.label = "S3Source";
  config.endpoint = source.endpoint();
  config.bucket = source.bucket();
  config.key_prefix = source.key_prefix();
  config.verify_ssl = s3_source_verify_ssl(source);
  if (source.has_region()) config.region = source.region();
  if (source.endpoint().empty()) {
    throw StorageRequestError(option_path + ".endpoint", option_path + ".endpoint is required");
  }
  if (source.bucket().empty()) {
    throw StorageRequestError(option_path + ".bucket", option_path + ".bucket is required");
  }
  const grpc::Status credentials = targets::resolve_s3_credentials(
      source.has_access_key() ? std::optional(source.access_key()) : std::nullopt,
      source.has_secret_key() ? std::optional(source.secret_key()) : std::nullopt,
      source.endpoint(), &config);
  if (!credentials.ok()) {
    throw StorageRequestError(option_path, option_path + ": " + credentials.error_message());
  }
  std::optional<targets::S3Client> client;
  try {
    client.emplace(config);
  } catch (const std::invalid_argument& bad) {
    throw StorageRequestError(option_path + ".endpoint", option_path + ": " + bad.what());
  }
  S3SourceStore store(std::move(*client));
  return fetch_single(store, source.key_prefix(), std::nullopt, option_path + ".key_prefix");
}

FetchedDocument fetch_http_source(const parsev1::HttpSource& source, const std::string& option_path,
                                  uint64_t max_bytes) {
  const std::string url_path = option_path + ".url";
  const std::string scheme = lowercase(source.url().substr(0, source.url().find("://")));
  if (source.url().find("://") == std::string::npos || (scheme != "http" && scheme != "https")) {
    throw StorageRequestError(url_path, url_path + " must be an http or https URL");
  }
  if (std::ranges::any_of(source.url(), [](unsigned char c) { return c <= 0x20 || c == 0x7F; })) {
    throw StorageRequestError(url_path, url_path + " carries whitespace or a control character");
  }
  std::vector<std::pair<std::string, std::string>> headers;
  static constexpr std::string_view kReserved[] = {
      "host", "content-length", "transfer-encoding", "connection", "expect", "te", "upgrade",
      "keep-alive", "proxy-connection", "trailer"};
  for (const auto& [name, value] : source.headers()) {
    const std::string where = option_path + ".headers[" + name + "]";
    if (name.empty() || !std::ranges::all_of(name, [](unsigned char c) { return header_token_char(c); })) {
      throw StorageRequestError(where, where + ": not a valid HTTP header name");
    }
    if (std::ranges::find(kReserved, lowercase(name)) != std::end(kReserved)) {
      throw StorageRequestError(where, where + ": set by the HTTP client and not accepted");
    }
    if (std::ranges::any_of(value, [](unsigned char c) { return (c < 0x20 && c != '\t') || c == 0x7F; })) {
      throw StorageRequestError(where, where + ": the value carries a control character");
    }
    headers.emplace_back(name, value);
  }
  // The caller's headers (often a credential) are meant for the origin the
  // caller named; a redirect elsewhere is followed without them.
  const std::string origin = url_origin(source.url());
  if (origin.empty()) throw StorageRequestError(url_path, url_path + " names no host");
  std::string url = source.url();
  for (int hop = 0;; ++hop) {
    HttpRequest request;
    request.url = url;
    request.max_body_bytes = max_bytes;
    if (url_origin(url) == origin) request.headers = headers;
    HttpResponse reply;
    try {
      reply = http_call(request, "HttpSource");
    } catch (const HttpPeerRefused& refused) {
      throw StorageRequestError(url_path, url_path + (hop == 0 ? "" : " (after a redirect)") +
                                              ": " + refused.what());
    }
    if (reply.status >= 300 && reply.status < 400 && !reply.redirect_url.empty()) {
      if (hop == kMaxRedirects) {
        throw std::runtime_error("HttpSource: more than " + std::to_string(kMaxRedirects) +
                                 " redirects");
      }
      const std::string next = lowercase(reply.redirect_url.substr(0, reply.redirect_url.find("://")));
      if (reply.redirect_url.find("://") == std::string::npos || (next != "http" && next != "https")) {
        throw std::runtime_error("HttpSource: redirected to a URL that is not http or https");
      }
      url = reply.redirect_url;
      continue;
    }
    if (reply.status >= 400 && reply.status < 500) {
      throw StorageRequestError(url_path, url_path + " answered HTTP " + std::to_string(reply.status));
    }
    if (!reply.ok()) {
      throw std::runtime_error("HttpSource: the server answered HTTP " + std::to_string(reply.status));
    }
    return FetchedDocument{.filename = http_source_filename(url, reply.header("content-disposition")),
                           .bytes = std::move(reply.body)};
  }
}

}  // namespace grparse::storage
