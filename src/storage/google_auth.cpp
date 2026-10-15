#include "google_auth.h"

#include <chrono>
#include <memory>
#include <stdexcept>

#include <google/protobuf/util/json_util.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "http_call.h"
#include "object_store.h"
#include "storage_wire.pb.h"

namespace grparse::storage {
namespace {

namespace wire = grparse::storage::wire;

std::string to_json(const google::protobuf::Message& message) {
  std::string json;
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  if (!google::protobuf::util::MessageToJsonString(message, &json, options).ok()) {
    throw std::runtime_error("could not render " + std::string(message.GetTypeName()));
  }
  return json;
}

std::string token_from_reply(const HttpResponse& reply, const std::string& what) {
  if (!reply.ok()) {
    throw std::runtime_error(what + ": the token endpoint answered HTTP " +
                             std::to_string(reply.status));
  }
  wire::OAuthTokenReply token;
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;
  if (!google::protobuf::util::JsonStringToMessage(reply.body, &token, options).ok() ||
      token.access_token().empty()) {
    throw std::runtime_error(what + ": the token endpoint's reply carries no access_token");
  }
  return token.access_token();
}

}  // namespace

std::string base64url(const std::string& bytes) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const uint32_t n = (static_cast<uint8_t>(bytes[i]) << 16) |
                       (static_cast<uint8_t>(bytes[i + 1]) << 8) | static_cast<uint8_t>(bytes[i + 2]);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
    out += kAlphabet[n & 63];
  }
  if (i + 1 == bytes.size()) {
    const uint32_t n = static_cast<uint8_t>(bytes[i]) << 16;
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
  } else if (i + 2 == bytes.size()) {
    const uint32_t n = (static_cast<uint8_t>(bytes[i]) << 16) | (static_cast<uint8_t>(bytes[i + 1]) << 8);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
  }
  return out;
}

std::string rs256_sign(const std::string& private_key_pem, const std::string& data) {
  const std::unique_ptr<BIO, decltype(&BIO_free)> bio(
      BIO_new_mem_buf(private_key_pem.data(), static_cast<int>(private_key_pem.size())), BIO_free);
  if (bio == nullptr) throw std::runtime_error("could not read the private key");
  const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
      PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
  if (key == nullptr) throw std::invalid_argument("private_key is not a PEM private key");
  const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                         EVP_MD_CTX_free);
  size_t length = 0;
  if (context == nullptr ||
      EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
      EVP_DigestSign(context.get(), nullptr, &length,
                     reinterpret_cast<const uint8_t*>(data.data()), data.size()) != 1) {
    throw std::invalid_argument("private_key cannot sign RS256");
  }
  std::string signature(length, '\0');
  if (EVP_DigestSign(context.get(), reinterpret_cast<uint8_t*>(signature.data()), &length,
                     reinterpret_cast<const uint8_t*>(data.data()), data.size()) != 1) {
    throw std::invalid_argument("private_key cannot sign RS256");
  }
  signature.resize(length);
  return signature;
}

std::string service_account_assertion(
    const ai::pipestream::parse::v1::GoogleCloudStorageServiceAccountKey& key,
    const std::string& scope, uint32_t now) {
  wire::JwtHeader header;
  header.set_alg("RS256");
  header.set_typ("JWT");
  header.set_kid(key.private_key_id());
  wire::JwtClaims claims;
  claims.set_iss(key.client_email());
  claims.set_scope(scope);
  claims.set_aud(key.token_uri());
  claims.set_iat(now);
  claims.set_exp(now + 3600);
  const std::string signing_input = base64url(to_json(header)) + "." + base64url(to_json(claims));
  return signing_input + "." + base64url(rs256_sign(key.private_key(), signing_input));
}

std::string service_account_access_token(
    const ai::pipestream::parse::v1::GoogleCloudStorageServiceAccountKey& key,
    const std::string& scope) {
  const auto now = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());
  HttpRequest request;
  request.method = "POST";
  request.url = key.token_uri();
  request.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
  request.body = "grant_type=" + percent_encode("urn:ietf:params:oauth:grant-type:jwt-bearer", false) +
                 "&assertion=" + percent_encode(service_account_assertion(key, scope, now), false);
  return token_from_reply(http_call(request, "google_cloud_storage token"),
                          "google_cloud_storage service account");
}

std::string refresh_access_token(const ai::pipestream::parse::v1::GoogleDriveCredentials& credentials,
                                 const std::string& refresh_token) {
  HttpRequest request;
  request.method = "POST";
  request.url = credentials.token_uri();
  request.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
  request.body = "grant_type=refresh_token&client_id=" + percent_encode(credentials.client_id(), false) +
                 "&client_secret=" + percent_encode(credentials.client_secret(), false) +
                 "&refresh_token=" + percent_encode(refresh_token, false);
  return token_from_reply(http_call(request, "google_drive token"), "google_drive refresh token");
}

}  // namespace grparse::storage
