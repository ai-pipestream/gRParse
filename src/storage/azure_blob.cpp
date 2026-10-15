#include "azure_blob.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <map>

#include "../targets/sha256.h"
#include "grparse/base64.h"
#include "http_call.h"

namespace grparse::storage {
namespace {

namespace parsev1 = ai::pipestream::parse::v1;

constexpr const char* kApiVersion = "2021-08-06";

std::string rfc1123_now() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm utc{};
  gmtime_r(&now, &utc);
  char buffer[64];
  std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &utc);
  return buffer;
}

std::string lowercase(std::string text) {
  std::ranges::transform(text, text.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// Unescapes the XML character entities a blob name can carry in a listing.
std::string xml_unescape(std::string_view text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      out += text[i];
      continue;
    }
    const size_t end = text.find(';', i);
    if (end == std::string_view::npos) {
      out += text.substr(i);
      break;
    }
    const std::string_view entity = text.substr(i + 1, end - i - 1);
    if (entity == "amp") out += '&';
    else if (entity == "lt") out += '<';
    else if (entity == "gt") out += '>';
    else if (entity == "quot") out += '"';
    else if (entity == "apos") out += '\'';
    else out += std::string(text.substr(i, end - i + 1));
    i = end;
  }
  return out;
}

// Every <tag>value</tag> in an XML document, in order (raw reply data from
// the store; the List Blobs schema nests no element of the same name).
std::vector<std::string> xml_values(const std::string& xml, const std::string& tag) {
  std::vector<std::string> values;
  const std::string open = "<" + tag + ">";
  const std::string close = "</" + tag + ">";
  size_t at = 0;
  while ((at = xml.find(open, at)) != std::string::npos) {
    const size_t start = at + open.size();
    const size_t end = xml.find(close, start);
    if (end == std::string::npos) break;
    values.push_back(xml_unescape(std::string_view(xml).substr(start, end - start)));
    at = end + close.size();
  }
  return values;
}

class AzureBlobStore final : public ObjectStore {
 public:
  AzureBlobStore(AzureConnection connection, std::string container)
      : connection_(std::move(connection)), container_(std::move(container)) {}

  std::vector<std::string> list(const std::string& prefix, std::optional<uint32_t> limit) override {
    std::vector<std::string> names;
    std::string marker;
    do {
      std::vector<std::pair<std::string, std::string>> query = {
          {"comp", "list"}, {"restype", "container"}};
      if (!prefix.empty()) query.emplace_back("prefix", prefix);
      if (!marker.empty()) query.emplace_back("marker", marker);
      if (limit.has_value()) {
        query.emplace_back("maxresults", std::to_string(*limit - std::min<size_t>(*limit, names.size())));
      }
      const HttpResponse reply = send("GET", "", query, "", "list blobs");
      if (!reply.ok()) {
        throw std::runtime_error("azure_blob: listing container '" + container_ +
                                 "' answered HTTP " + std::to_string(reply.status));
      }
      for (std::string& name : xml_values(reply.body, "Name")) names.push_back(std::move(name));
      const std::vector<std::string> next = xml_values(reply.body, "NextMarker");
      marker = next.empty() ? std::string() : next.front();
    } while (!marker.empty() && (!limit.has_value() || names.size() < *limit));
    if (limit.has_value() && names.size() > *limit) names.resize(*limit);
    return names;
  }

  std::string get(const std::string& name) override {
    const HttpResponse reply = send("GET", name, {}, "", "get blob");
    if (!reply.ok()) {
      throw std::runtime_error("azure_blob: reading '" + name + "' answered HTTP " +
                               std::to_string(reply.status));
    }
    return reply.body;
  }

  WrittenObject put(const std::string& name, const std::string& bytes) override {
    const HttpResponse reply = send("PUT", name, {}, bytes, "put blob");
    if (!reply.ok()) {
      throw std::runtime_error("azure_blob: writing '" + name + "' answered HTTP " +
                               std::to_string(reply.status));
    }
    return WrittenObject{.key = name, .etag = reply.header("etag"), .size_bytes = bytes.size(),
                         .store_id = std::string()};
  }

 private:
  HttpResponse send(const std::string& method, const std::string& blob,
                    std::vector<std::pair<std::string, std::string>> query,
                    const std::string& body, const std::string& what) const {
    std::string path_tail = "/" + percent_encode(container_, false);
    if (!blob.empty()) path_tail += "/" + percent_encode(blob, true);
    // The URL path as sent: the endpoint's own path (the emulator's account
    // segment) and the container and blob.
    const size_t scheme_end = connection_.blob_endpoint.find("://");
    const size_t path_start = connection_.blob_endpoint.find('/', scheme_end + 3);
    const std::string endpoint_path =
        path_start == std::string::npos ? std::string() : connection_.blob_endpoint.substr(path_start);
    const std::string resource_path = endpoint_path + path_tail;

    std::vector<std::pair<std::string, std::string>> ms = {{"x-ms-date", rfc1123_now()},
                                                           {"x-ms-version", kApiVersion}};
    const std::string content_type = method == "PUT" ? "application/octet-stream" : "";
    if (method == "PUT") ms.emplace_back("x-ms-blob-type", "BlockBlob");
    std::ranges::sort(query);
    const std::string to_sign = azure_string_to_sign(method, body.size(), content_type, ms,
                                                     connection_.account_name, resource_path, query);
    const std::string mac = targets::hmac_sha256(connection_.account_key, to_sign);
    const std::string signature = encode_base64(mac.data(), mac.size());

    HttpRequest request;
    request.method = method;
    request.url = connection_.blob_endpoint + path_tail;
    for (size_t i = 0; i < query.size(); ++i) {
      request.url += (i == 0 ? "?" : "&") + query[i].first + "=" + percent_encode(query[i].second, false);
    }
    request.headers = ms;
    if (!content_type.empty()) request.headers.emplace_back("Content-Type", content_type);
    request.headers.emplace_back("Authorization",
                                 "SharedKey " + connection_.account_name + ":" + signature);
    request.body = body;
    return http_call(request, "azure_blob: " + what);
  }

  AzureConnection connection_;
  std::string container_;
};

}  // namespace

std::string azure_string_to_sign(const std::string& method, size_t content_length,
                                 const std::string& content_type,
                                 const std::vector<std::pair<std::string, std::string>>& ms_headers,
                                 const std::string& account, const std::string& resource_path,
                                 const std::vector<std::pair<std::string, std::string>>& query) {
  std::string out = method + "\n";
  out += "\n";  // Content-Encoding
  out += "\n";  // Content-Language
  out += (content_length == 0 ? std::string() : std::to_string(content_length)) + "\n";
  out += "\n";  // Content-MD5
  out += content_type + "\n";
  out += "\n";  // Date (x-ms-date is used)
  out += "\n\n\n\n\n";  // If-Modified-Since, If-Match, If-None-Match, If-Unmodified-Since, Range
  std::map<std::string, std::string> headers;
  for (const auto& [name, value] : ms_headers) headers[lowercase(name)] = value;
  for (const auto& [name, value] : headers) out += name + ":" + value + "\n";
  out += "/" + account + resource_path;
  std::map<std::string, std::string> params;
  for (const auto& [name, value] : query) params[lowercase(name)] = value;
  for (const auto& [name, value] : params) out += "\n" + name + ":" + value;
  return out;
}

AzureConnection parse_azure_connection_string(const std::string& connection_string,
                                              const std::string& account_name,
                                              const std::string& option_path) {
  std::map<std::string, std::string> parts;
  size_t at = 0;
  while (at < connection_string.size()) {
    size_t end = connection_string.find(';', at);
    if (end == std::string::npos) end = connection_string.size();
    const std::string part = connection_string.substr(at, end - at);
    at = end + 1;
    if (part.empty()) continue;
    const size_t equals = part.find('=');
    if (equals == std::string::npos) {
      throw StorageRequestError(option_path + ".connection_string",
                                option_path + ".connection_string has a part without '='");
    }
    parts[part.substr(0, equals)] = part.substr(equals + 1);
  }
  if (parts.contains("UseDevelopmentStorage")) {
    throw StorageRequestError(option_path + ".connection_string",
                              option_path + ".connection_string: UseDevelopmentStorage names the "
                              "server's own loopback emulator and is not accepted; give "
                              "AccountName, AccountKey and BlobEndpoint");
  }
  AzureConnection connection;
  connection.account_name = parts["AccountName"];
  if (connection.account_name.empty() || parts["AccountKey"].empty()) {
    throw StorageRequestError(option_path + ".connection_string",
                              option_path + ".connection_string must carry AccountName and "
                              "AccountKey (Shared Key)");
  }
  if (connection.account_name != account_name) {
    throw StorageRequestError(option_path + ".account_name",
                              option_path + ".account_name does not match the connection "
                              "string's AccountName");
  }
  connection.account_key = decode_base64(parts["AccountKey"]);
  if (connection.account_key.empty()) {
    throw StorageRequestError(option_path + ".connection_string",
                              option_path + ".connection_string AccountKey is not base64");
  }
  if (const auto blob = parts.find("BlobEndpoint"); blob != parts.end() && !blob->second.empty()) {
    connection.blob_endpoint = blob->second;
  } else {
    const std::string protocol =
        parts.contains("DefaultEndpointsProtocol") ? parts["DefaultEndpointsProtocol"] : "https";
    const std::string suffix =
        parts.contains("EndpointSuffix") ? parts["EndpointSuffix"] : "core.windows.net";
    connection.blob_endpoint = protocol + "://" + connection.account_name + ".blob." + suffix;
  }
  while (connection.blob_endpoint.ends_with('/')) connection.blob_endpoint.pop_back();
  if (!connection.blob_endpoint.starts_with("https://") &&
      !connection.blob_endpoint.starts_with("http://")) {
    throw StorageRequestError(option_path + ".connection_string",
                              option_path + ".connection_string BlobEndpoint must be http or https");
  }
  return connection;
}

std::unique_ptr<ObjectStore> open_azure_blob(const parsev1::AzureBlobCoordinates& coordinates,
                                             const std::string& option_path) {
  if (coordinates.container().empty()) {
    throw StorageRequestError(option_path + ".container", option_path + ".container is required");
  }
  if (coordinates.account_name().empty()) {
    throw StorageRequestError(option_path + ".account_name",
                              option_path + ".account_name is required");
  }
  AzureConnection connection = parse_azure_connection_string(
      coordinates.connection_string(), coordinates.account_name(), option_path);
  return std::make_unique<AzureBlobStore>(std::move(connection), coordinates.container());
}

}  // namespace grparse::storage
