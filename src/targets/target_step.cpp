#include "target_step.h"

#include <stdexcept>
#include <string>
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

grpc::Status deliver_s3(const parsev1::S3Target& target, const docv1::Document& document,
                        const parsev1::DocumentExports& exports,
                        parsev1::TargetResult* result) {
  S3Config config;
  config.endpoint = target.endpoint();
  config.access_key = target.access_key();
  config.secret_key = target.secret_key();
  config.bucket = target.bucket();
  config.key_prefix = target.key_prefix();
  // Verification stays on unless the caller explicitly turned it off; an
  // absent field must never mean insecure.
  config.verify_ssl = target.has_verify_ssl() ? target.verify_ssl() : true;

  std::vector<UploadedObject> objects;
  try {
    objects = upload_bundle(config, build_bundle(document, exports));
  } catch (const std::invalid_argument& incomplete) {
    // The target's own fields, not the store: a caller can fix these.
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, incomplete.what());
  } catch (const std::exception& refused) {
    // Whatever the store did, the message carries the key that failed and
    // nothing that was signed with.
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, refused.what());
  }
  for (const auto& object : objects) {
    auto* stored = result->add_objects();
    stored->set_key(object.key);
    stored->set_etag(object.etag);
    stored->set_size_bytes(object.size_bytes);
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
