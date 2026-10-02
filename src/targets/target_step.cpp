#include "target_step.h"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "bundle.h"
#include "s3_client.h"
#include "s3_uploader.h"
#include "zip_writer.h"

namespace parsev1 = ai::pipestream::parse::v1;
namespace docv1 = ai::pipestream::document::v1;

namespace grparse::targets {
namespace {

grpc::Status unimplemented(const std::string& name) {
  return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                      "ConvertSource does not implement target '" + name + "'");
}

grpc::Status deliver_zip(const docv1::Document& document,
                         const parsev1::DocumentExports& exports,
                         parsev1::TargetResult* result) {
  result->set_archive(write_zip(build_bundle(document, exports)));
  return grpc::Status::OK;
}

grpc::Status resolve_s3_credentials(const parsev1::S3Target& target, S3Config* config) {
  const bool has_access = target.has_access_key();
  const bool has_secret = target.has_secret_key();
  if (has_access != has_secret) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "S3Target: access_key and secret_key must be provided together");
  }
  if (has_access) {
    if (target.access_key().empty() || target.secret_key().empty()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "S3Target: access_key and secret_key must be non-empty when set");
    }
    config->access_key = target.access_key();
    config->secret_key = target.secret_key();
    return grpc::Status::OK;
  }
  // The server's own identity signs for a caller-chosen endpoint and bucket
  // only when the deployment opted in: otherwise any caller could write
  // wherever that identity reaches, or point the endpoint at itself and
  // collect the access key ID and session token from the headers.
  const char* ambient = std::getenv("GRPARSE_S3_AMBIENT_CREDENTIALS");
  if (ambient == nullptr || std::string_view(ambient) != "1") {
    return grpc::Status(
        grpc::StatusCode::INVALID_ARGUMENT,
        "S3Target omitted credentials and ambient credentials are disabled "
        "(GRPARSE_S3_AMBIENT_CREDENTIALS=1 enables them)");
  }
  const char* access = std::getenv("AWS_ACCESS_KEY_ID");
  const char* secret = std::getenv("AWS_SECRET_ACCESS_KEY");
  if (access == nullptr || access[0] == '\0' || secret == nullptr || secret[0] == '\0') {
    return grpc::Status(
        grpc::StatusCode::INVALID_ARGUMENT,
        "S3Target omitted credentials and AWS_ACCESS_KEY_ID and "
        "AWS_SECRET_ACCESS_KEY are not both set");
  }
  config->access_key = access;
  config->secret_key = secret;
  if (const char* token = std::getenv("AWS_SESSION_TOKEN");
      token != nullptr && token[0] != '\0') {
    config->session_token = token;
  }
  return grpc::Status::OK;
}

grpc::Status deliver_s3(const parsev1::S3Target& target, const docv1::Document& document,
                        const parsev1::DocumentExports& exports,
                        parsev1::TargetResult* result) {
  S3Config config;
  config.endpoint = target.endpoint();
  config.bucket = target.bucket();
  config.key_prefix = target.key_prefix();
  // Verification stays on unless the caller explicitly turned it off; an
  // absent field must never mean insecure.
  config.verify_ssl = target.has_verify_ssl() ? target.verify_ssl() : true;
  if (target.has_region()) {
    config.region = target.region();
  }
  const grpc::Status credentials = resolve_s3_credentials(target, &config);
  if (!credentials.ok()) return credentials;

  const auto report = [result](const std::vector<UploadedObject>& objects) {
    for (const auto& object : objects) {
      auto* stored = result->add_objects();
      stored->set_key(object.key);
      stored->set_etag(object.etag);
      stored->set_size_bytes(object.size_bytes);
    }
  };
  try {
    report(upload_bundle(config, build_bundle(document, exports)));
  } catch (const std::invalid_argument& incomplete) {
    // The target's own fields, not the store: a caller can fix these.
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, incomplete.what());
  } catch (const UploadFailure& refused) {
    // The members written before the refusal are in the store regardless:
    // they are reported, so the caller can clean up or retry.
    report(refused.written());
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, refused.what());
  } catch (const std::exception& refused) {
    // Whatever the store did, the message carries the key that failed and
    // nothing that was signed with.
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, refused.what());
  }
  return grpc::Status::OK;
}

}  // namespace

bool needs_delivery(const parsev1::Target& target) {
  switch (target.target_case()) {
    case parsev1::Target::kInbody:
    case parsev1::Target::TARGET_NOT_SET:
      return false;
    default:
      return true;
  }
}

grpc::Status deliver(const parsev1::Target& target, const docv1::Document& document,
                     const parsev1::DocumentExports& exports, parsev1::TargetResult* result) {
  switch (target.target_case()) {
    case parsev1::Target::kZip:
      return deliver_zip(document, exports, result);
    case parsev1::Target::kS3:
      return deliver_s3(target.s3(), document, exports, result);
    case parsev1::Target::kPut:
      return unimplemented("put");
    case parsev1::Target::kPresignedUrl:
      return unimplemented("presigned_url");
    case parsev1::Target::kInbody:
    case parsev1::Target::TARGET_NOT_SET:
      return grpc::Status::OK;
  }
  return unimplemented("unknown");
}

}  // namespace grparse::targets
