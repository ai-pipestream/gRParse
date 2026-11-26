// The repository target against a real document repository: the service
// that serves ai.protomolt.proto.repo.v2.DocumentService, not the in-process
// stand-in of repository_target_test.cpp.  Everything asserted here is read
// back from the repository over its own read RPCs, so the test fails when a
// save "succeeded" without the bytes being there, and when the Document is
// there but the repository stored it without decoding it.
//
// The repository must have the schema of ai.pipestream.document.v1.Document.
// protomolt's reads it over gRPC server reflection from the servers its
// DOCUMENT_PLATFORM_REFLECTED_SCHEMA_TARGETS names: a running gRParse, or
// any server that serves the Document's descriptor.
//
// It needs a running repository and skips (77) without one:
//   GRPARSE_E2E_REPOSITORY            host:port of the repository's gRPC
//                                     listener; unset skips the test
//   GRPARSE_E2E_REPOSITORY_API_TOKEN  the credential the repository accepts
//   GRPARSE_E2E_REPOSITORY_ACCOUNT    an account the repository knows, with
//                                     its intake drive (the repository's
//                                     DOCUMENT_PLATFORM_SEED_ACCOUNT_ID)
//   GRPARSE_E2E_REPOSITORY_TLS        "1" or "true" to dial with TLS
// Each run writes documents under ids of its own, so a repository can be
// reused between runs.

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "../src/targets/bundle.h"
#include "../src/targets/repository_target.h"
#include "../src/targets/sha256.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "ai/protomolt/proto/repo/v1/document.pb.h"
#include "ai/protomolt/proto/repo/v2/document_service.grpc.pb.h"
#include "ai/protomolt/proto/repo/v2/manifest.pb.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;
namespace repov1 = ai::protomolt::proto::repo::v1;
namespace repov2 = ai::protomolt::proto::repo::v2;
namespace targets = grparse::targets;

using grparse_test::add_paragraph;
using grparse_test::base_document;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

constexpr const char* kDatasource = "grparse-e2e";
constexpr const char* kDocumentTypeUrl =
    "type.googleapis.com/ai.pipestream.document.v1.Document";

std::string environment(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

struct Repository {
  targets::RepositoryConfig config;
  std::string account;
  std::unique_ptr<repov2::DocumentService::Stub> stub;
  // Distinguishes this run's documents from every earlier run's.
  std::string run;

  void prepare(grpc::ClientContext* context) const {
    context->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(60));
    if (!config.api_token.empty()) context->AddMetadata("api_token", config.api_token);
  }
};

Repository repository_from_environment() {
  Repository repository;
  repository.config.address = environment("GRPARSE_E2E_REPOSITORY");
  repository.config.api_token = environment("GRPARSE_E2E_REPOSITORY_API_TOKEN");
  const std::string tls = environment("GRPARSE_E2E_REPOSITORY_TLS");
  repository.config.tls = tls == "1" || tls == "true";
  repository.config.deadline = std::chrono::seconds(60);
  repository.account = environment("GRPARSE_E2E_REPOSITORY_ACCOUNT");
  require(!repository.account.empty(),
          "GRPARSE_E2E_REPOSITORY_ACCOUNT names an account the repository knows");
  repository.stub = repov2::DocumentService::NewStub(grpc::CreateChannel(
      repository.config.address,
      repository.config.tls ? grpc::SslCredentials(grpc::SslCredentialsOptions())
                            : grpc::InsecureChannelCredentials()));
  repository.run =
      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count()) +
      "-" + std::to_string(::getpid());
  return repository;
}

parsev1::ConvertDocumentResponse conversion(const std::string& text) {
  parsev1::ConvertDocumentResponse converted;
  converted.set_status(parsev1::CONVERSION_STATUS_PARTIAL_SUCCESS);
  docv1::Document document = base_document("sample.pdf");
  document.set_schema_name("pipestream_document_v1");
  add_paragraph(&document, "#/body", text);
  // Large enough that document.pb crosses several data frames.
  add_paragraph(&document, "#/body", std::string(700 * 1024, 'x'));
  *converted.mutable_document()->mutable_doc() = document;
  converted.mutable_document()->set_filename("sample.pdf");
  converted.mutable_document()->mutable_exports()->set_md(text);
  auto* error = converted.add_errors();
  error->set_module_name("ocr");
  error->set_error_message("page 3 was unreadable");
  return converted;
}

std::vector<targets::BundleFile> bundle(const parsev1::ConvertDocumentResponse& converted) {
  parsev1::TargetResult unused;
  return targets::build_bundle(converted, &unused);
}

std::string member(const std::vector<targets::BundleFile>& files, const std::string& path) {
  for (const auto& file : files) {
    if (file.path == path) return file.bytes;
  }
  throw std::runtime_error("the bundle holds no " + path);
}

parsev1::RepositoryTarget target_for(const Repository& repository, const std::string& name) {
  parsev1::RepositoryTarget target;
  target.set_account_id(repository.account);
  target.set_datasource_id(kDatasource);
  target.set_doc_id("grparse-e2e-" + repository.run + "-" + name);
  target.set_connector_id("grparse-e2e");
  target.set_create(true);
  return target;
}

repov1::NodeAddress address_of(const parsev1::RepositorySave& saved) {
  repov1::NodeAddress address;
  address.set_doc_id(saved.doc_id());
  address.set_graph_address_id(saved.graph_address_id());
  address.set_account_id(saved.account_id());
  address.set_graph_id(saved.graph_id());
  return address;
}

struct ReadBack {
  grpc::Status status;
  repov2::BlobHeader header;
  std::string bytes;
};

// One blob of one revision, as the repository serves it.
ReadBack read_blob(const Repository& repository, const repov1::NodeAddress& address,
                   int64_t revision, const std::string& blob_id) {
  repov2::ReadBlobRequest request;
  *request.mutable_address() = address;
  request.mutable_revision()->set_revision(revision);
  request.set_blob_id(blob_id);
  grpc::ClientContext context;
  repository.prepare(&context);
  const auto stream = repository.stub->ReadBlob(&context, request);
  ReadBack read;
  repov2::ReadBlobResponse frame;
  while (stream->Read(&frame)) {
    if (frame.has_header()) {
      read.header = frame.header();
    } else {
      read.bytes += frame.data();
    }
  }
  read.status = stream->Finish();
  return read;
}

// The blob must be in the repository at `revision` with exactly `bytes`.
void require_stored(const Repository& repository, const parsev1::RepositorySave& saved,
                    int64_t revision, const std::string& blob_id, const std::string& bytes,
                    const std::string& what) {
  const ReadBack read = read_blob(repository, address_of(saved), revision, blob_id);
  require(read.status.ok(), what + " reads back: " + read.status.error_message());
  require_equal(read.header.revision(), revision, what + " is read at the committed revision");
  require_equal(read.header.total_length(), static_cast<int64_t>(bytes.size()),
                what + " has the bundle member's length");
  require_equal(read.header.sha256(), targets::sha256_hex(bytes),
                what + " has the bundle member's SHA-256");
  require(read.bytes == bytes, what + " is the bundle member, byte for byte");
}

// The parser results of one revision, as a typed read serves them.
repov2::GetDocumentResponse read_parsed(const Repository& repository,
                                        const repov1::NodeAddress& address, int64_t revision,
                                        const std::string& what) {
  repov2::GetDocumentRequest request;
  *request.mutable_address() = address;
  request.mutable_revision()->set_revision(revision);
  request.mutable_selection()->add_selectors()->set_part(repov2::DOCUMENT_PART_PARSED);
  repov2::GetDocumentResponse response;
  grpc::ClientContext context;
  repository.prepare(&context);
  const grpc::Status status = repository.stub->GetDocument(&context, request, &response);
  require(status.ok(), what + " reads back typed: " + status.error_message());
  require_equal(response.manifest().revision(), revision,
                what + " is read at the committed revision");
  return response;
}

// The Document must be in the repository at `revision` as the grparse parser
// result, with exactly `bytes`, decoded by the repository with a schema it
// retained. Returns the read for what else a case wants from it.
repov2::GetDocumentResponse require_document(const Repository& repository,
                                             const parsev1::RepositorySave& saved,
                                             int64_t revision, const std::string& bytes,
                                             const std::string& what) {
  const repov2::GetDocumentResponse read =
      read_parsed(repository, address_of(saved), revision, what);
  const auto found = read.document().parser_results().find("grparse");
  require(found != read.document().parser_results().end(),
          what + " is the document's grparse parser result");
  const repov1::ParserResult& result = found->second;
  require_equal(result.parser_name(), "grparse", what + " names its parser");
  require(!result.parser_version().empty(), what + " names the version that ran");
  require_equal(result.document().shape().type_url(), kDocumentTypeUrl,
                what + " is typed as the Document");
  require(result.document().shape().value() == bytes,
          what + " is the bundle's document.pb, byte for byte");

  // Typed, not parked: the repository decoded the shape with a schema of its
  // own and says which.
  const repov2::RevisionValidation& validation = read.validation();
  require_equal(static_cast<int>(validation.status()),
                static_cast<int>(repov2::VALIDATION_STATUS_TYPED_VALID),
                what + " was validated typed: " + validation.ShortDebugString());
  require_equal(validation.opaque_occurrences_size(), 0, what + " holds nothing opaque");
  const std::string digest = targets::sha256_hex(bytes);
  bool decoded = false;
  for (const auto& occurrence : validation.resolved_occurrences()) {
    if (occurrence.schema().type_url() != kDocumentTypeUrl) continue;
    if (occurrence.payload_sha256() != digest) continue;
    require_equal(occurrence.payload_length(), static_cast<int64_t>(bytes.size()),
                  what + ": the decoded payload's length");
    require_equal(occurrence.schema().descriptor_sha256().size(), size_t{64},
                  what + ": the schema the repository retained for it");
    decoded = true;
  }
  require(decoded, what + " is recorded as decoded with the Document's schema: " +
                       validation.ShortDebugString());
  return read;
}

parsev1::RepositorySave save(const Repository& repository, const targets::RepositoryConfig& config,
                             const parsev1::RepositoryTarget& target,
                             const std::vector<targets::BundleFile>& files,
                             const std::string& what) {
  parsev1::TargetResult result;
  const grpc::Status status = targets::deliver_repository(config, target, files, &result);
  require(status.ok(), what + " commits: " + status.error_message() + " " +
                           result.repository_failure().ShortDebugString());
  require(result.has_repository(), what + " reports its accession");
  require(!result.has_repository_failure(), what + " reports no failure");
  require(result.repository().revision() > 0, what + " names a committed revision");
  require_equal(result.repository().doc_id(), target.doc_id(), what + " names the document");
  require_equal(result.repository().account_id(), repository.account, what + " names the account");
  return result.repository();
}

// ---- the cases ------------------------------------------------------------

void verify_a_create_is_in_the_repository_typed_and_byte_for_byte(const Repository& repository) {
  const auto converted = conversion("the first conversion");
  const auto files = bundle(converted);
  const parsev1::RepositoryTarget target = target_for(repository, "create");

  const auto saved = save(repository, repository.config, target, files, "the create");
  require(!saved.replayed(), "a first save is not a replay");
  require_equal(saved.graph_address_id(), kDatasource, "the intake address is the datasource");
  require_equal(saved.graph_id(), "intake:" + repository.account, "the intake graph");
  const repov2::GetDocumentResponse read = require_document(
      repository, saved, saved.revision(), member(files, "document.pb"), "the Document");
  require_stored(repository, saved, saved.revision(), "grparse:response.pb",
                 member(files, "response.pb"), "response.pb");

  // What comes back is what was converted, and says how the conversion went.
  const repov1::ParserResult& result = read.document().parser_results().at("grparse");
  docv1::Document stored;
  require(result.document().shape().UnpackTo(&stored), "the stored shape unpacks as a Document");
  require(stored.SerializeAsString() == converted.document().doc().SerializeAsString(),
          "the stored Document is the converted one");
  require_equal(static_cast<int>(result.status()), static_cast<int>(repov1::PARSE_STATUS_PARTIAL),
                "a partial conversion is a partial parse");
  require_equal(result.error(), "ocr: page 3 was unreadable", "with the conversion's error");
  // The Document is no longer also a file.
  const ReadBack file =
      read_blob(repository, address_of(saved), saved.revision(), "grparse:document.pb");
  require(!file.status.ok(), "the Document is not stored a second time as a blob");

  // The same conversion again is the same save, not a second revision.
  const auto again = save(repository, repository.config, target, files, "the repeated save");
  require(again.replayed(), "the same conversion saved twice replays");
  require_equal(again.revision(), saved.revision(), "a replay names the revision it committed");
  require_equal(again.operation_id(), saved.operation_id(), "a replay keeps its operation id");

  // Another conversion that still expects no document is refused, typed,
  // and names the revision that is there.
  parsev1::TargetResult refused;
  const grpc::Status conflict = targets::deliver_repository(
      repository.config, target, bundle(conversion("a different conversion")), &refused);
  require(!conflict.ok(), "a create over an existing document does not commit");
  require_equal(static_cast<int>(conflict.error_code()),
                static_cast<int>(grpc::StatusCode::UNAVAILABLE),
                "a refused save is a delivery failure: " + conflict.error_message());
  require(!refused.has_repository(), "a refused save reports no accession");
  require_equal(static_cast<int>(refused.repository_failure().reason()),
                static_cast<int>(parsev1::REPOSITORY_FAILURE_REASON_REVISION_CONFLICT),
                "the refusal is a revision conflict: " +
                    refused.repository_failure().ShortDebugString());
  require_equal(refused.repository_failure().current_revision(), saved.revision(),
                "the conflict names the revision the repository holds");
  require_document(repository, saved, saved.revision(), member(files, "document.pb"),
                   "the Document after the refused save");
}

void verify_an_update_replaces_its_own_content_and_keeps_another_writers(
    const Repository& repository) {
  // Another writer creates the document: a file of its own and its own
  // parser's result.
  parsev1::RepositoryTarget target = target_for(repository, "update");
  const std::string note = "kept by someone else";
  repov2::SaveDocumentRequest other;
  other.set_operation_id("grparse-e2e-" + repository.run + "-other-writer");
  other.mutable_owner()->mutable_intake()->set_datasource_id(kDatasource);
  other.mutable_target()->set_use_datasource_id(kDatasource);
  other.mutable_expected()->set_none(true);
  other.mutable_document()->set_doc_id(target.doc_id());
  other.mutable_document()->mutable_ownership()->set_account_id(repository.account);
  other.mutable_document()->mutable_ownership()->set_datasource_id(kDatasource);
  auto* foreign = other.mutable_document()->mutable_blob_bag()->mutable_blobs()->add_blob();
  foreign->set_blob_id("other:note.txt");
  foreign->set_data(note);
  foreign->set_mime_type("text/plain");
  foreign->set_filename("note.txt");
  repov1::ParserResult theirs;
  theirs.set_parser_name("other-parser");
  theirs.set_parser_version("7.2");
  theirs.set_status(repov1::PARSE_STATUS_OK);
  theirs.set_config_fingerprint("other-config");
  (*other.mutable_document()->mutable_parser_results())["other-parser"] = theirs;
  for (const auto part : {repov2::DOCUMENT_PART_CORE, repov2::DOCUMENT_PART_BLOBS,
                          repov2::DOCUMENT_PART_PARSED}) {
    auto* write = other.add_writes();
    write->mutable_selector()->set_part(part);
    write->set_action(repov2::WRITE_ACTION_PUT);
  }
  repov2::SaveDocumentResponse other_saved;
  grpc::ClientContext context;
  repository.prepare(&context);
  const grpc::Status other_status = repository.stub->SaveDocument(&context, other, &other_saved);
  require(other_status.ok(), "the other writer's save commits: " + other_status.error_message());
  const int64_t theirs_revision = other_saved.accession().revision();

  // The first parse is saved onto the revision the other writer made.
  const auto first_files = bundle(conversion("before the update"));
  target.set_revision(theirs_revision);
  const auto parsed = save(repository, repository.config, target, first_files, "the first parse");
  require_equal(parsed.previous_revision(), theirs_revision,
                "the first parse names the revision it replaced");

  // A re-parse names the revision the first one committed.
  const auto second_files = bundle(conversion("after the update"));
  target.set_revision(parsed.revision());
  const auto updated = save(repository, repository.config, target, second_files, "the update");
  require(updated.revision() > parsed.revision(), "an update is a new revision");
  require_equal(updated.previous_revision(), parsed.revision(),
                "the update names the revision it replaced");
  const repov2::GetDocumentResponse read =
      require_document(repository, updated, updated.revision(),
                       member(second_files, "document.pb"), "the updated Document");
  require_stored(repository, updated, updated.revision(), "grparse:response.pb",
                 member(second_files, "response.pb"), "the updated response.pb");
  require_stored(repository, updated, updated.revision(), "other:note.txt", note,
                 "the other writer's blob after the update");
  require_equal(read.document().parser_results_size(), 2,
                "the other parser's result and this target's, one entry each");
  require(read.document().parser_results().at("other-parser").SerializeAsString() ==
              theirs.SerializeAsString(),
          "the other parser's result is kept as it was");
  // History is intact: the first parse's revision still serves its Document.
  require_document(repository, parsed, parsed.revision(), member(first_files, "document.pb"),
                   "the first parse's Document");

  // The revision the update replaced is now stale.
  parsev1::TargetResult refused;
  const grpc::Status stale = targets::deliver_repository(
      repository.config, target, bundle(conversion("a late writer")), &refused);
  require(!stale.ok(), "a save against a replaced revision does not commit");
  require_equal(static_cast<int>(refused.repository_failure().reason()),
                static_cast<int>(parsev1::REPOSITORY_FAILURE_REASON_REVISION_CONFLICT),
                "the stale save is a revision conflict: " +
                    refused.repository_failure().ShortDebugString());
  require_equal(refused.repository_failure().current_revision(), updated.revision(),
                "the conflict names the current revision");
}

void verify_a_response_over_the_inline_budget_takes_a_second_accession(
    const Repository& repository) {
  const auto files = bundle(conversion("a conversion with a large response"));
  targets::RepositoryConfig small = repository.config;
  small.inline_budget_bytes = 16;
  const parsev1::RepositoryTarget target = target_for(repository, "two-accessions");

  const auto saved = save(repository, small, target, files, "the two-accession save");
  require(saved.revision() >= 2, "the second accession is a second revision");
  require_document(repository, saved, saved.revision(), member(files, "document.pb"),
                   "the Document after the second accession");
  require_stored(repository, saved, saved.revision(), "grparse:response.pb",
                 member(files, "response.pb"), "the streamed response.pb");

  const auto again = save(repository, small, target, files, "the repeated two-accession save");
  require(again.replayed(), "both accessions replay");
  require_equal(again.revision(), saved.revision(), "the replay names the same revision");
}

void verify_a_wrong_credential_is_refused_typed(const Repository& repository) {
  targets::RepositoryConfig wrong = repository.config;
  wrong.api_token = "not-the-repositorys-token";
  parsev1::TargetResult result;
  const grpc::Status status = targets::deliver_repository(
      wrong, target_for(repository, "wrong-token"), bundle(conversion("never stored")), &result);
  require(!status.ok(), "a save with a wrong credential does not commit");
  require(!result.has_repository(), "it reports no accession");
  require_equal(static_cast<int>(result.repository_failure().reason()),
                static_cast<int>(parsev1::REPOSITORY_FAILURE_REASON_NOT_AUTHORIZED),
                "the refusal is NOT_AUTHORIZED: " + result.repository_failure().ShortDebugString());
}

void verify_an_unknown_account_is_refused_typed(const Repository& repository) {
  parsev1::RepositoryTarget target = target_for(repository, "unknown-account");
  target.set_account_id("grparse-e2e-no-such-account-" + repository.run);
  parsev1::TargetResult result;
  const grpc::Status status = targets::deliver_repository(
      repository.config, target, bundle(conversion("never stored")), &result);
  require(!status.ok(), "a save for an account the repository does not know does not commit");
  require(!result.has_repository(), "it reports no accession");
  require(result.has_repository_failure(), "it reports a typed failure");
  require(result.repository_failure().reason() !=
              parsev1::REPOSITORY_FAILURE_REASON_UNSPECIFIED,
          "the failure names a reason: " + result.repository_failure().ShortDebugString());
  require(result.repository_failure().grpc_status_code() != 0,
          "the failure carries the repository's status code");
}

}  // namespace

int main() {
  if (std::getenv("GRPARSE_E2E_REPOSITORY") == nullptr) {
    std::cout << "SKIP: GRPARSE_E2E_REPOSITORY is not set (needs a running document repository)\n";
    return 77;
  }
  try {
    const Repository repository = repository_from_environment();
    verify_a_create_is_in_the_repository_typed_and_byte_for_byte(repository);
    verify_an_update_replaces_its_own_content_and_keeps_another_writers(repository);
    verify_a_response_over_the_inline_budget_takes_a_second_accession(repository);
    verify_a_wrong_credential_is_refused_typed(repository);
    verify_an_unknown_account_is_refused_typed(repository);
  } catch (const std::exception& failure) {
    std::cerr << "FAIL: " << failure.what() << "\n";
    return 1;
  }
  std::cout << "repository-target-e2e-test: ok\n";
  return 0;
}
