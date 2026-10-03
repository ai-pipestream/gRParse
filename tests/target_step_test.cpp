// The Target step of a conversion: which targets ask for delivery beyond the
// response body, which the server serves, and what a target whose own fields
// are incomplete answers.  The bundle, the signer, and a live store are
// covered by targets_test.cpp; this file pins the step that dispatches them.

#include <cstdlib>
#include <optional>
#include <string>

#include <grpcpp/grpcpp.h>

#include "../src/targets/target_step.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;
namespace targets = grparse::targets;

using grparse_test::add_paragraph;
using grparse_test::base_document;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

docv1::Document sample_document() {
  docv1::Document document = base_document("sample.pdf");
  document.set_schema_name("pipestream_document_v1");
  add_paragraph(&document, "#/body", "one paragraph");
  return document;
}

parsev1::DocumentExports sample_exports() {
  parsev1::DocumentExports exports;
  exports.set_md("one paragraph");
  return exports;
}

void verify_only_a_delivering_target_asks_for_the_step() {
  parsev1::Target unset;
  require(!targets::needs_delivery(unset), "an unset target asks for nothing");

  parsev1::Target in_body;
  in_body.mutable_inbody();
  require(!targets::needs_delivery(in_body),
          "an explicit in-body target is the same request as no target");

  for (const auto& setter : {+[](parsev1::Target* target) { target->mutable_zip(); },
                             +[](parsev1::Target* target) { target->mutable_s3(); },
                             +[](parsev1::Target* target) { target->mutable_put(); },
                             +[](parsev1::Target* target) { target->mutable_presigned_url(); }}) {
    parsev1::Target target;
    setter(&target);
    require(targets::needs_delivery(target),
            "every target beyond the response body asks for the step");
  }
}

void verify_no_target_at_all_is_a_clean_no_op() {
  parsev1::TargetResult result;
  parsev1::Target target;
  const grpc::Status status =
      targets::deliver(target, sample_document(), sample_exports(), &result);
  require(status.ok(), "an unset target succeeds without doing anything");
  require(!result.has_archive(), "an unset target writes no archive");
  require_equal(result.objects_size(), 0, "an unset target writes no objects");

  target.mutable_inbody();
  parsev1::TargetResult in_body_result;
  require(targets::deliver(target, sample_document(), sample_exports(), &in_body_result).ok(),
          "an explicit in-body target succeeds without doing anything");
  require(!in_body_result.has_archive(), "an in-body target writes no archive");
}

void verify_a_zip_target_fills_the_archive() {
  parsev1::Target target;
  target.mutable_zip();
  parsev1::TargetResult result;
  const grpc::Status status =
      targets::deliver(target, sample_document(), sample_exports(), &result);
  require(status.ok(), "a zip target is served: " + status.error_message());
  require(result.has_archive(), "a zip target fills the archive field");
  require(result.archive().starts_with("PK\x03\x04"),
          "the archive is a zip, by its local file header");
  require_equal(result.objects_size(), 0, "a zip target writes no store objects");
}

void verify_the_archive_is_a_pure_function_of_its_input() {
  parsev1::Target target;
  target.mutable_zip();
  parsev1::TargetResult first;
  parsev1::TargetResult second;
  require(targets::deliver(target, sample_document(), sample_exports(), &first).ok(),
          "the first delivery succeeds");
  require(targets::deliver(target, sample_document(), sample_exports(), &second).ok(),
          "the second delivery succeeds");
  require_equal(first.archive() == second.archive(), true,
                "two deliveries of the same document produce byte-identical archives");
}

void verify_the_declared_but_unserved_targets_are_refused_by_name() {
  const struct {
    void (*set)(parsev1::Target*);
    std::string name;
  } cases[] = {
      {[](parsev1::Target* target) { target->mutable_put()->set_url("https://x/y"); }, "put"},
      {[](parsev1::Target* target) { target->mutable_presigned_url(); }, "presigned_url"},
  };
  for (const auto& one : cases) {
    parsev1::Target target;
    one.set(&target);
    parsev1::TargetResult result;
    const grpc::Status status =
        targets::deliver(target, sample_document(), sample_exports(), &result);
    require_equal(static_cast<int>(status.error_code()),
                  static_cast<int>(grpc::StatusCode::UNIMPLEMENTED),
                  "a declared but unserved target is UNIMPLEMENTED");
    require_equal(status.error_message(),
                  "ConvertSource does not implement target '" + one.name + "'",
                  "the refusal names the target the caller asked for");
  }
}

void verify_an_incomplete_store_target_is_the_callers_fault() {
  const char* saved_access = std::getenv("AWS_ACCESS_KEY_ID");
  const std::string access_copy = saved_access ? saved_access : "";
  const bool had_access = saved_access != nullptr;
  const char* saved_secret = std::getenv("AWS_SECRET_ACCESS_KEY");
  const std::string secret_copy = saved_secret ? saved_secret : "";
  const bool had_secret = saved_secret != nullptr;
  unsetenv("AWS_ACCESS_KEY_ID");
  unsetenv("AWS_SECRET_ACCESS_KEY");
  const struct {
    void (*set)(parsev1::S3Target*);
    std::string what;
  } cases[] = {
      {[](parsev1::S3Target* s3) {
         s3->set_bucket("b");
         s3->set_access_key("a");
         s3->set_secret_key("s");
       },
       "a store target with no endpoint"},
      {[](parsev1::S3Target* s3) {
         s3->set_endpoint("https://store.test");
         s3->set_access_key("a");
         s3->set_secret_key("s");
       },
       "a store target with no bucket"},
      {[](parsev1::S3Target* s3) {
         s3->set_endpoint("https://store.test");
         s3->set_bucket("b");
       },
       "a store target with no credentials"},
  };
  for (const auto& one : cases) {
    parsev1::Target target;
    one.set(target.mutable_s3());
    parsev1::TargetResult result;
    const grpc::Status status =
        targets::deliver(target, sample_document(), sample_exports(), &result);
    require_equal(static_cast<int>(status.error_code()),
                  static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT),
                  one.what + " is INVALID_ARGUMENT, not a store failure");
    require(!status.error_message().empty(), one.what + " says what is missing");
    require_equal(result.objects_size(), 0, one.what + " writes no objects");
  }
  if (had_access) setenv("AWS_ACCESS_KEY_ID", access_copy.c_str(), 1);
  if (had_secret) setenv("AWS_SECRET_ACCESS_KEY", secret_copy.c_str(), 1);
}

void verify_s3_credentials_are_paired() {
  struct EnvSlot {
    const char* name;
    std::optional<std::string> previous;
    explicit EnvSlot(const char* name) : name(name) {
      if (const char* current = std::getenv(name)) previous = current;
      unsetenv(name);
    }
    ~EnvSlot() {
      if (previous.has_value()) setenv(name, previous->c_str(), 1);
      else unsetenv(name);
    }
  };
  EnvSlot access("AWS_ACCESS_KEY_ID");
  EnvSlot secret("AWS_SECRET_ACCESS_KEY");
  EnvSlot token("AWS_SESSION_TOKEN");
  EnvSlot ambient("GRPARSE_S3_AMBIENT_CREDENTIALS");
  EnvSlot ambient_endpoints("GRPARSE_S3_AMBIENT_ENDPOINTS");

  auto deliver_s3 = [](void (*fill)(parsev1::S3Target*)) {
    parsev1::Target target;
    fill(target.mutable_s3());
    parsev1::TargetResult result;
    return targets::deliver(target, sample_document(), sample_exports(), &result);
  };

  const grpc::Status one_key = deliver_s3([](parsev1::S3Target* s3) {
    s3->set_endpoint("https://127.0.0.1:9");
    s3->set_bucket("bucket");
    s3->set_access_key("only-access");
  });
  require_equal(static_cast<int>(one_key.error_code()),
                static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT),
                "one S3 key without the other is INVALID_ARGUMENT");
  require(one_key.error_message().contains("provided together"),
          one_key.error_message());

  const grpc::Status empty = deliver_s3([](parsev1::S3Target* s3) {
    s3->set_endpoint("https://127.0.0.1:9");
    s3->set_bucket("bucket");
    s3->set_access_key("");
    s3->set_secret_key("");
  });
  require_equal(static_cast<int>(empty.error_code()),
                static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT),
                "empty S3 keys are INVALID_ARGUMENT");

  setenv("AWS_ACCESS_KEY_ID", "AKIAEXAMPLEKEY", 1);
  setenv("AWS_SECRET_ACCESS_KEY", "s3cr3t-value-that-must-never-print", 1);
  const grpc::Status not_opted_in = deliver_s3([](parsev1::S3Target* s3) {
    s3->set_endpoint("https://127.0.0.1:9");
    s3->set_bucket("bucket");
  });
  require_equal(static_cast<int>(not_opted_in.error_code()),
                static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT),
                "the server's own credentials never sign without the deployment's opt-in");
  require(not_opted_in.error_message().contains("GRPARSE_S3_AMBIENT_CREDENTIALS"),
          not_opted_in.error_message());
  unsetenv("AWS_ACCESS_KEY_ID");
  unsetenv("AWS_SECRET_ACCESS_KEY");

  setenv("GRPARSE_S3_AMBIENT_CREDENTIALS", "1", 1);
  // The opt-in alone signs for no endpoint: only the listed ones, and only
  // over https.
  const grpc::Status unlisted = deliver_s3([](parsev1::S3Target* s3) {
    s3->set_endpoint("https://127.0.0.1:9");
    s3->set_bucket("bucket");
  });
  require_equal(static_cast<int>(unlisted.error_code()),
                static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT),
                "ambient credentials never sign for an unlisted endpoint");
  require(unlisted.error_message().contains("GRPARSE_S3_AMBIENT_ENDPOINTS"),
          unlisted.error_message());
  setenv("GRPARSE_S3_AMBIENT_ENDPOINTS", "s3.example.test, HTTPS://127.0.0.1:9 ,", 1);
  const grpc::Status other_port = deliver_s3([](parsev1::S3Target* s3) {
    s3->set_endpoint("https://127.0.0.1:10");
    s3->set_bucket("bucket");
  });
  require(other_port.error_message().contains("GRPARSE_S3_AMBIENT_ENDPOINTS"),
          "a listed host on another port is not listed: " + other_port.error_message());
  const grpc::Status plain_http = deliver_s3([](parsev1::S3Target* s3) {
    s3->set_endpoint("http://127.0.0.1:9");
    s3->set_bucket("bucket");
  });
  require_equal(static_cast<int>(plain_http.error_code()),
                static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT),
                "ambient credentials never sign a cleartext request");
  require(plain_http.error_message().contains("not https"), plain_http.error_message());

  const grpc::Status missing_env = deliver_s3([](parsev1::S3Target* s3) {
    s3->set_endpoint("https://127.0.0.1:9");
    s3->set_bucket("bucket");
  });
  require_equal(static_cast<int>(missing_env.error_code()),
                static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT),
                "omitted S3 keys without the environment pair are INVALID_ARGUMENT");
  require(missing_env.error_message().contains("AWS_ACCESS_KEY_ID"),
          missing_env.error_message());

  setenv("AWS_ACCESS_KEY_ID", "AKIAEXAMPLEKEY", 1);
  setenv("AWS_SECRET_ACCESS_KEY", "s3cr3t-value-that-must-never-print", 1);
  const grpc::Status from_env = deliver_s3([](parsev1::S3Target* s3) {
    s3->set_endpoint("https://127.0.0.1:9");
    s3->set_bucket("bucket");
  });
  require(from_env.error_code() != grpc::StatusCode::INVALID_ARGUMENT,
          "a complete environment pair is accepted as credentials: " +
              from_env.error_message());
  require(!from_env.error_message().contains("s3cr3t-value-that-must-never-print"),
          "the failure must not carry the secret key: " + from_env.error_message());
  require(!from_env.error_message().contains("AKIAEXAMPLEKEY"),
          "the failure must not carry the access key: " + from_env.error_message());

  // A session token is a bearer credential: an http endpoint is refused
  // before anything is sent, rather than handed it in cleartext.
  setenv("AWS_SESSION_TOKEN", "session-token-that-must-never-print", 1);
  const grpc::Status cleartext = deliver_s3([](parsev1::S3Target* s3) {
    s3->set_endpoint("http://127.0.0.1:9");
    s3->set_bucket("bucket");
  });
  require_equal(static_cast<int>(cleartext.error_code()),
                static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT),
                "a session token over http is INVALID_ARGUMENT");
  require(!cleartext.error_message().contains("session-token-that-must-never-print"),
          "the refusal must not carry the token: " + cleartext.error_message());
}

void verify_a_failed_delivery_leaks_no_credentials() {
  parsev1::Target target;
  auto* s3 = target.mutable_s3();
  s3->set_bucket("bucket");
  s3->set_access_key("AKIAEXAMPLEKEY");
  s3->set_secret_key("s3cr3t-value-that-must-never-print");
  parsev1::TargetResult result;
  const grpc::Status status =
      targets::deliver(target, sample_document(), sample_exports(), &result);
  require(!status.ok(), "a store target with no endpoint fails");
  require(!status.error_message().contains("s3cr3t-value-that-must-never-print"),
          "the failure must not carry the secret key: " + status.error_message());
  require(!status.error_message().contains("AKIAEXAMPLEKEY"),
          "the failure must not carry the access key: " + status.error_message());
}

}  // namespace

int main() {
  return grparse_test::run_test_main("target-step-test", "ok", {
      verify_only_a_delivering_target_asks_for_the_step,
      verify_no_target_at_all_is_a_clean_no_op,
      verify_a_zip_target_fills_the_archive,
      verify_the_archive_is_a_pure_function_of_its_input,
      verify_the_declared_but_unserved_targets_are_refused_by_name,
      verify_an_incomplete_store_target_is_the_callers_fault,
      verify_s3_credentials_are_paired,
      verify_a_failed_delivery_leaks_no_credentials,
  });
}
