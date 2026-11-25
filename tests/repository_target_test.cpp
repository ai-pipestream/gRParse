// The repository target (src/targets/repository_target.h): a conversion
// saved through ai.protomolt.proto.repo.v2.DocumentService.  The repository
// here is an in-process stand-in built from the same vendored stubs, and it
// keeps the rules of the real one that a client can get wrong: a credential
// on every call, content only for the parts a save writes, no
// client-supplied storage reference or write stamp on a blob, a blob without
// bytes only when the expected revision holds it, the declared length and
// SHA-256 of the streamed bytes, compare-and-swap on the expected revision,
// and replay by operation id.  What it stores is read back and compared
// byte for byte with the bundle, so a save that "succeeded" without the
// bytes arriving fails here.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "../src/storage/connectors.h"
#include "../src/targets/bundle.h"
#include "../src/targets/repository_target.h"
#include "../src/targets/sha256.h"
#include "../src/targets/target_step.h"
#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "ai/protomolt/proto/repo/v1/document.pb.h"
#include "ai/protomolt/proto/repo/v2/document_service.grpc.pb.h"
#include "ai/protomolt/proto/repo/v2/errors.pb.h"
#include "google/rpc/status.pb.h"
#include "support/check.h"
#include "support/document_builder.h"

namespace docv1 = ai::pipestream::document::v1;
namespace parsev1 = ai::pipestream::parse::v1;
namespace repov1 = ai::protomolt::proto::repo::v1;
namespace repov2 = ai::protomolt::proto::repo::v2;
namespace targets = grparse::targets;
namespace storage = grparse::storage;

using grparse_test::add_paragraph;
using grparse_test::base_document;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

constexpr const char* kToken = "repository-test-token";

class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    const char* before = std::getenv(name);
    if (before != nullptr) before_ = before;
    if (value == nullptr) {
      ::unsetenv(name);
    } else {
      ::setenv(name, value, 1);
    }
  }
  ~ScopedEnv() {
    if (before_.has_value()) {
      ::setenv(name_, before_->c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }

 private:
  const char* name_;
  std::optional<std::string> before_;
};

template <typename Detail>
grpc::Status status_with(grpc::StatusCode code, const std::string& message,
                         const Detail& detail) {
  google::rpc::Status rich;
  rich.set_code(static_cast<int>(code));
  rich.set_message(message);
  if (!rich.add_details()->PackFrom(detail)) {
    throw std::runtime_error("the stand-in repository could not pack a status detail");
  }
  return grpc::Status(code, message, rich.SerializeAsString());
}

grpc::Status malformed(const std::string& message) {
  repov2::SaveRejection rejection;
  rejection.set_reason(repov2::REJECTION_REASON_MALFORMED_REQUEST);
  rejection.set_message(message);
  return status_with(grpc::StatusCode::INVALID_ARGUMENT, message, rejection);
}

std::string address_key(const repov1::NodeAddress& address) {
  return address.doc_id() + "|" + address.graph_address_id() + "|" + address.account_id() + "|" +
         address.graph_id();
}

bool blank(const std::string& text) {
  return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

// The stand-in repository.
class FakeRepository final : public repov2::DocumentService::Service {
 public:
  struct StoredBlob {
    repov1::Blob blob;  // as sent, without bytes
    std::string bytes;
  };
  struct Revision {
    repov1::Document core;  // doc_id and ownership
    std::vector<StoredBlob> blobs;
    repov2::SaveOwner owner;
    std::string drive;
  };

  // What the next upload does instead of saving, once.
  void refuse_upload(int upload_number, grpc::Status status) {
    std::lock_guard lock(mutex_);
    refusals_[upload_number] = std::move(status);
  }
  void answer_another_operation() {
    std::lock_guard lock(mutex_);
    answer_another_operation_ = true;
  }

  // A document some other writer saved: one revision holding `blob_id`.
  void seed(const repov1::NodeAddress& address, const std::string& blob_id,
            const std::string& bytes) {
    std::lock_guard lock(mutex_);
    Revision revision;
    revision.core.set_doc_id(address.doc_id());
    revision.core.mutable_ownership()->set_account_id(address.account_id());
    revision.core.mutable_ownership()->set_datasource_id(address.graph_address_id());
    StoredBlob stored;
    stored.blob.set_blob_id(blob_id);
    stored.blob.set_filename("source.pdf");
    stored.blob.set_mime_type("application/pdf");
    stored.blob.set_size_bytes(static_cast<int64_t>(bytes.size()));
    stored.blob.set_checksum(targets::sha256_hex(bytes));
    stored.blob.set_checksum_type(repov1::CHECKSUM_TYPE_SHA256);
    stored.bytes = bytes;
    revision.blobs.push_back(std::move(stored));
    documents_[address_key(address)].push_back(std::move(revision));
  }

  std::vector<Revision> revisions(const repov1::NodeAddress& address) const {
    std::lock_guard lock(mutex_);
    const auto found = documents_.find(address_key(address));
    return found == documents_.end() ? std::vector<Revision>() : found->second;
  }
  int uploads() const {
    std::lock_guard lock(mutex_);
    return uploads_;
  }
  int reads() const {
    std::lock_guard lock(mutex_);
    return reads_;
  }
  int data_frames() const {
    std::lock_guard lock(mutex_);
    return data_frames_;
  }
  std::vector<repov2::UploadHeader> headers() const {
    std::lock_guard lock(mutex_);
    return headers_;
  }

  grpc::Status GetDocument(grpc::ServerContext* context, const repov2::GetDocumentRequest* request,
                           repov2::GetDocumentResponse* response) override {
    if (grpc::Status credential = check_credential(*context); !credential.ok()) return credential;
    std::lock_guard lock(mutex_);
    ++reads_;
    const auto found = documents_.find(address_key(request->address()));
    const int64_t wanted = request->revision().has_revision()
                               ? request->revision().revision()
                               : (found == documents_.end()
                                      ? 0
                                      : static_cast<int64_t>(found->second.size()));
    if (found == documents_.end() || wanted <= 0 ||
        wanted > static_cast<int64_t>(found->second.size())) {
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "Document revision not found");
    }
    const Revision& revision = found->second[static_cast<size_t>(wanted - 1)];
    // A typed read: the claim check filled, the bytes elided, and the
    // repository's own write stamp on every blob.
    auto* bag = response->mutable_document()->mutable_blob_bag()->mutable_blobs();
    for (const StoredBlob& stored : revision.blobs) {
      repov1::Blob* blob = bag->add_blob();
      *blob = stored.blob;
      blob->mutable_storage_ref()->set_drive_name("intake");
      blob->mutable_storage_ref()->set_object_key("objects/" + stored.blob.checksum());
      blob->mutable_written()->set_revision(wanted);
    }
    *response->mutable_manifest()->mutable_address() = request->address();
    response->mutable_manifest()->set_revision(wanted);
    return grpc::Status::OK;
  }

  grpc::Status UploadDocument(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<repov2::UploadDocumentResponse, repov2::UploadDocumentRequest>*
          stream) override {
    if (grpc::Status credential = check_credential(*context); !credential.ok()) return credential;
    repov2::UploadDocumentRequest frame;
    if (!stream->Read(&frame) || !frame.has_header()) {
      return malformed("The first upload frame must be a header");
    }
    const repov2::UploadHeader header = frame.header();
    const repov2::SaveDocumentRequest& save = header.save();

    std::unique_lock lock(mutex_);
    const int upload_number = ++uploads_;
    headers_.push_back(header);
    if (const auto refusal = refusals_.find(upload_number); refusal != refusals_.end()) {
      return refusal->second;
    }

    // --- the save's shape -------------------------------------------------
    if (blank(save.operation_id())) return malformed("operation_id is required");
    if (!save.owner().has_intake() || blank(save.owner().intake().datasource_id())) {
      return malformed("an owner names an intake origin or a step");
    }
    std::set<repov2::DocumentPart> puts;
    for (const auto& write : save.writes()) {
      if (write.action() != repov2::WRITE_ACTION_PUT || !write.selector().has_part() ||
          !puts.insert(write.selector().part()).second) {
        return malformed("Unsupported or duplicate write");
      }
    }
    const bool writes_core = puts.contains(repov2::DOCUMENT_PART_CORE);
    const bool core_content = !save.document().doc_id().empty() || save.document().has_ownership();
    if (core_content && !writes_core) {
      return malformed("Content for CORE is present but the part is not written");
    }
    if (!puts.contains(repov2::DOCUMENT_PART_BLOBS)) {
      return malformed("Uploading a raw blob requires a BLOBS PUT write");
    }
    repov1::NodeAddress address;
    if (save.target().has_use_datasource_id()) {
      if (!writes_core || blank(save.document().doc_id())) {
        return malformed(
            "an intake datasource target needs a CORE PUT with a non-blank document doc_id");
      }
      if (blank(save.document().ownership().account_id())) {
        return malformed("An intake datasource target needs document ownership with an account");
      }
      if (save.document().ownership().datasource_id() != save.target().use_datasource_id()) {
        return malformed("The target datasource differs from the document's ownership");
      }
      address.set_doc_id(save.document().doc_id());
      address.set_graph_address_id(save.target().use_datasource_id());
      address.set_account_id(save.document().ownership().account_id());
      address.set_graph_id("intake:" + address.account_id());
    } else if (save.target().has_address()) {
      address = save.target().address();
      if (blank(address.doc_id()) || blank(address.graph_address_id()) ||
          blank(address.account_id()) || blank(address.graph_id())) {
        return malformed("a target address needs a non-blank doc_id, graph_address_id, "
                         "account_id and graph_id");
      }
    } else {
      return malformed("a target names an address or an intake datasource");
    }
    if (save.owner().intake().datasource_id() != address.graph_address_id()) {
      repov2::SaveRejection denied;
      denied.set_reason(repov2::REJECTION_REASON_WRITE_DENIED);
      denied.set_message("The intake owner differs from the document's datasource");
      return status_with(grpc::StatusCode::PERMISSION_DENIED, denied.message(), denied);
    }
    if (!header.has_blob_id() || blank(header.blob_id())) {
      return malformed("Uploaded blob_id must not be blank");
    }
    if (!header.has_streamed()) {
      return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "only streamed bodies here");
    }

    // --- the blob bag -----------------------------------------------------
    std::vector<repov1::Blob> bag;
    if (save.document().blob_bag().has_blob()) bag.push_back(save.document().blob_bag().blob());
    for (const auto& blob : save.document().blob_bag().blobs().blob()) bag.push_back(blob);
    std::set<std::string> ids;
    bool streamed_listed = false;
    for (const auto& blob : bag) {
      if (blob.has_storage_ref()) {
        return malformed("Blob " + blob.blob_id() +
                         " names a storage_ref; clients cannot supply storage references");
      }
      if (blob.has_written()) {
        return malformed("Blob " + blob.blob_id() +
                         " names written; the repository records the write that binds a blob");
      }
      if (blank(blob.blob_id()) || !ids.insert(blob.blob_id()).second) {
        return malformed("Every blob needs a unique, non-blank blob_id");
      }
      if (blob.checksum_type() != repov1::CHECKSUM_TYPE_UNSPECIFIED &&
          blob.checksum_type() != repov1::CHECKSUM_TYPE_SHA256) {
        return malformed("the repository verifies and records SHA-256 only");
      }
      if (blob.blob_id() == header.blob_id()) {
        if (blob.has_data()) return malformed("The streamed blob must not carry inline data");
        streamed_listed = true;
      }
    }
    if (!streamed_listed) return malformed("The uploaded blob is not in the blob bag");

    // --- replay, reuse, expected revision ---------------------------------
    const std::string outcome_key = address.account_id() + "|" + save.operation_id();
    const std::string intent = targets::sha256_hex(header.SerializeAsString());
    if (const auto recorded = outcomes_.find(outcome_key); recorded != outcomes_.end()) {
      if (recorded->second.first != intent) {
        repov2::OperationReuse reuse;
        reuse.set_operation_id(save.operation_id());
        reuse.set_recorded_intent_sha256(recorded->second.first);
        reuse.set_request_intent_sha256(intent);
        return status_with(grpc::StatusCode::ALREADY_EXISTS,
                           "Operation ID was used with a different intent", reuse);
      }
      repov2::UploadDocumentResponse replayed;
      *replayed.mutable_replayed() = recorded->second.second;
      replayed.mutable_replayed()->set_replayed(true);
      lock.unlock();
      stream->Write(replayed);
      return grpc::Status::OK;
    }
    std::vector<Revision>& history = documents_[address_key(address)];
    const int64_t current = static_cast<int64_t>(history.size());
    const bool expectation_holds = save.expected().has_none()
                                       ? current == 0
                                       : (save.expected().has_revision() &&
                                          save.expected().revision() == current);
    if (!expectation_holds) {
      repov2::RevisionConflict conflict;
      *conflict.mutable_address() = address;
      *conflict.mutable_expected() = save.expected();
      conflict.set_current_revision(current);
      return status_with(grpc::StatusCode::ABORTED, "Expected revision is stale", conflict);
    }
    if (current == 0 && (!writes_core || !save.document().has_ownership())) {
      return malformed("Creating a document needs a CORE PUT with ownership");
    }
    const Revision* base = current == 0 ? nullptr : &history.back();
    for (const auto& blob : bag) {
      if (blob.has_data() || blob.blob_id() == header.blob_id()) continue;
      const bool held = base != nullptr &&
                        std::ranges::any_of(base->blobs, [&blob](const StoredBlob& stored) {
                          return stored.blob.blob_id() == blob.blob_id();
                        });
      if (!held) {
        return malformed("Blob " + blob.blob_id() +
                         " has no inline data and names no raw blob of the expected revision");
      }
    }
    lock.unlock();

    // --- the bytes --------------------------------------------------------
    repov2::UploadDocumentResponse accepted;
    accepted.mutable_accepted();
    if (!stream->Write(accepted)) return grpc::Status(grpc::StatusCode::CANCELLED, "gone");
    std::string body;
    int frames = 0;
    while (stream->Read(&frame)) {
      if (!frame.has_data()) return malformed("Only data frames follow an accepted header");
      body += frame.data();
      ++frames;
    }
    const auto mismatch = [](const std::string& message) {
      repov2::SaveRejection rejection;
      rejection.set_reason(repov2::REJECTION_REASON_UPLOAD_MISMATCH);
      rejection.set_message(message);
      return status_with(grpc::StatusCode::INVALID_ARGUMENT, message, rejection);
    };
    if (header.has_declared_length() &&
        header.declared_length() != static_cast<int64_t>(body.size())) {
      return mismatch("Uploaded stream length differs from declared length");
    }
    if (!header.expected_sha256().empty() &&
        header.expected_sha256() != targets::sha256_hex(body)) {
      return mismatch("Uploaded stream SHA-256 differs from the expected one");
    }

    // --- the commit -------------------------------------------------------
    lock.lock();
    data_frames_ += frames;
    std::vector<Revision>& committed_history = documents_[address_key(address)];
    if (static_cast<int64_t>(committed_history.size()) != current) {
      return grpc::Status(grpc::StatusCode::ABORTED, "Expected revision is stale");
    }
    Revision next;
    if (writes_core) {
      next.core.set_doc_id(save.document().doc_id());
      *next.core.mutable_ownership() = save.document().ownership();
    } else {
      next.core = committed_history.back().core;
    }
    next.owner = save.owner();
    next.drive = save.drive();
    for (const auto& blob : bag) {
      StoredBlob stored;
      stored.blob = blob;
      stored.blob.clear_content();
      if (blob.has_data()) {
        stored.bytes = blob.data();
      } else if (blob.blob_id() == header.blob_id()) {
        stored.bytes = body;
      } else {
        for (const StoredBlob& held : committed_history.back().blobs) {
          if (held.blob.blob_id() == blob.blob_id()) stored.bytes = held.bytes;
        }
      }
      if (blob.size_bytes() != 0 && blob.size_bytes() != static_cast<int64_t>(stored.bytes.size())) {
        return malformed("Blob " + blob.blob_id() + " size_bytes disagrees with its bytes");
      }
      if (blob.has_checksum() && blob.checksum() != targets::sha256_hex(stored.bytes)) {
        return malformed("Blob " + blob.blob_id() + " checksum disagrees with its bytes");
      }
      next.blobs.push_back(std::move(stored));
    }
    committed_history.push_back(std::move(next));

    repov2::SaveDocumentResponse saved;
    saved.set_operation_id(answer_another_operation_ ? "someone-elses-operation"
                                                     : save.operation_id());
    answer_another_operation_ = false;
    auto* accession = saved.mutable_accession();
    *accession->mutable_address() = address;
    accession->set_revision(current + 1);
    accession->set_previous_revision(current);
    accession->mutable_committed_at()->set_seconds(1'800'000'000 + current);
    outcomes_[outcome_key] = {intent, saved};
    lock.unlock();
    repov2::UploadDocumentResponse answer;
    *answer.mutable_saved() = saved;
    stream->Write(answer);
    return grpc::Status::OK;
  }

 private:
  static grpc::Status check_credential(const grpc::ServerContext& context) {
    const auto& metadata = context.client_metadata();
    const auto found = metadata.find("api_token");
    if (found == metadata.end() ||
        std::string(found->second.data(), found->second.size()) != kToken) {
      return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Missing API token 'api_token'");
    }
    return grpc::Status::OK;
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::vector<Revision>> documents_;
  std::map<std::string, std::pair<std::string, repov2::SaveDocumentResponse>> outcomes_;
  std::map<int, grpc::Status> refusals_;
  std::vector<repov2::UploadHeader> headers_;
  bool answer_another_operation_ = false;
  int uploads_ = 0;
  int reads_ = 0;
  int data_frames_ = 0;
};

class RepositoryFixture {
 public:
  RepositoryFixture() {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(&repository);
    server_ = builder.BuildAndStart();
    if (server_ == nullptr || port_ == 0) {
      throw std::runtime_error("the stand-in repository failed to start");
    }
  }
  ~RepositoryFixture() { server_->Shutdown(); }

  std::string address() const { return "127.0.0.1:" + std::to_string(port_); }
  targets::RepositoryConfig config() const {
    targets::RepositoryConfig config;
    config.address = address();
    config.api_token = kToken;
    config.deadline = std::chrono::seconds(20);
    return config;
  }

  FakeRepository repository;

 private:
  int port_ = 0;
  std::unique_ptr<grpc::Server> server_;
};

// A document large enough that its bytes cross several data frames.
docv1::Document sample_document(const std::string& text) {
  docv1::Document document = base_document("sample.pdf");
  document.set_schema_name("pipestream_document_v1");
  add_paragraph(&document, "#/body", text);
  add_paragraph(&document, "#/body", std::string(700 * 1024, 'x'));
  return document;
}

parsev1::ConvertDocumentResponse sample_conversion(const std::string& text = "one paragraph") {
  parsev1::ConvertDocumentResponse converted;
  converted.set_status(parsev1::CONVERSION_STATUS_PARTIAL_SUCCESS);
  *converted.mutable_document()->mutable_doc() = sample_document(text);
  converted.mutable_document()->set_filename("sample.pdf");
  converted.mutable_document()->mutable_exports()->set_md(text);
  auto* error = converted.add_errors();
  error->set_module_name("ocr");
  error->set_error_message("page 3 was unreadable");
  return converted;
}

parsev1::RepositoryTarget sample_target() {
  parsev1::RepositoryTarget target;
  target.set_account_id("acme");
  target.set_datasource_id("contracts");
  target.set_doc_id("doc-1");
  target.set_create(true);
  return target;
}

repov1::NodeAddress sample_address() {
  repov1::NodeAddress address;
  address.set_doc_id("doc-1");
  address.set_graph_address_id("contracts");
  address.set_account_id("acme");
  address.set_graph_id("intake:acme");
  return address;
}

std::string member(const std::vector<targets::BundleFile>& files, const std::string& path) {
  for (const auto& file : files) {
    if (file.path == path) return file.bytes;
  }
  throw std::runtime_error("the bundle holds no " + path);
}

const FakeRepository::StoredBlob& blob(const FakeRepository::Revision& revision,
                                       std::string_view blob_id) {
  for (const auto& stored : revision.blobs) {
    if (stored.blob.blob_id() == blob_id) return stored;
  }
  throw std::runtime_error("the revision holds no blob " + std::string(blob_id));
}

parsev1::OptionRejection rejection_of(const grpc::Status& status) {
  google::rpc::Status rich;
  require(rich.ParseFromString(status.error_details()), "the refusal carries status details");
  require_equal(rich.details_size(), 1, "the refusal carries one typed detail");
  parsev1::OptionRejection rejection;
  require(rich.details(0).UnpackTo(&rejection), "the detail is an OptionRejection");
  return rejection;
}

// ---- before any work ------------------------------------------------------

void verify_a_server_with_no_repository_refuses_the_target_by_name() {
  ScopedEnv address("GRPARSE_REPOSITORY_TARGET", nullptr);
  parsev1::Target target;
  *target.mutable_repository() = sample_target();
  require(targets::needs_delivery(target), "a repository target asks for the target step");
  require_equal(storage::target_kind(target), "repository", "the target's kind name");

  const grpc::Status status = storage::check_target_allowed(target);
  require_equal(static_cast<int>(status.error_code()),
                static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT),
                "an unconfigured repository refuses the request before the conversion");
  const parsev1::OptionRejection rejection = rejection_of(status);
  require_equal(rejection.option_path(), "target.repository", "the rejected option");
  require_equal(static_cast<int>(rejection.reason()),
                static_cast<int>(parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED),
                "the refusal is policy, not a malformed request");
  require(status.error_message().find("GRPARSE_REPOSITORY_TARGET") != std::string::npos,
          "the refusal names the setting that turns the target on");

  // The step itself refuses the same way: nothing falls back to another
  // target, and nothing is reported as delivered.
  parsev1::TargetResult result;
  const grpc::Status delivered = targets::deliver(target, sample_conversion(), &result);
  require_equal(static_cast<int>(delivered.error_code()),
                static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT),
                "the target step refuses an unconfigured repository too");
  require(!result.has_repository() && !result.has_archive() && result.objects_size() == 0,
          "a refused repository target delivers nothing anywhere");
}

void verify_a_listed_target_policy_must_name_the_repository() {
  RepositoryFixture fixture;
  const std::string address = fixture.address();
  ScopedEnv configured("GRPARSE_REPOSITORY_TARGET", address.c_str());
  parsev1::Target target;
  *target.mutable_repository() = sample_target();
  {
    ScopedEnv listed("GRPARSE_ALLOWED_TARGET_TYPES", "zip,s3");
    const grpc::Status status = storage::check_target_allowed(target);
    require(!status.ok(), "a target list that omits the repository refuses it");
    require_equal(static_cast<int>(rejection_of(status).reason()),
                  static_cast<int>(parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED),
                  "the omission is a policy refusal");
  }
  {
    ScopedEnv listed("GRPARSE_ALLOWED_TARGET_TYPES", "zip, repository");
    require(storage::check_target_allowed(target).ok(),
            "a target list that names the repository allows it");
  }
  {
    ScopedEnv listed("GRPARSE_ALLOWED_TARGET_TYPES", nullptr);
    require(storage::check_target_allowed(target).ok(),
            "with no list, configuring a repository is what allows the target");
  }
  require_equal(fixture.repository.uploads() + fixture.repository.reads(), 0,
                "checking a target dials nothing");
}

void verify_an_incomplete_target_is_refused_field_by_field() {
  RepositoryFixture fixture;
  const std::string address = fixture.address();
  ScopedEnv configured("GRPARSE_REPOSITORY_TARGET", address.c_str());
  const struct {
    void (*edit)(parsev1::RepositoryTarget*);
    std::string path;
  } cases[] = {
      {[](parsev1::RepositoryTarget* t) { t->set_account_id("  "); },
       "target.repository.account_id"},
      {[](parsev1::RepositoryTarget* t) { t->clear_datasource_id(); },
       "target.repository.datasource_id"},
      {[](parsev1::RepositoryTarget* t) { t->set_doc_id(""); }, "target.repository.doc_id"},
      {[](parsev1::RepositoryTarget* t) { t->set_doc_id(std::string(201, 'd')); },
       "target.repository.doc_id"},
      {[](parsev1::RepositoryTarget* t) { t->set_connector_id(" "); },
       "target.repository.connector_id"},
      {[](parsev1::RepositoryTarget* t) { t->set_drive(""); }, "target.repository.drive"},
      {[](parsev1::RepositoryTarget* t) { t->set_operation_id(std::string(181, 'o')); },
       "target.repository.operation_id"},
      {[](parsev1::RepositoryTarget* t) { t->clear_expected(); }, "target.repository.expected"},
      {[](parsev1::RepositoryTarget* t) { t->set_create(false); }, "target.repository.create"},
      {[](parsev1::RepositoryTarget* t) { t->set_revision(0); }, "target.repository.revision"},
  };
  for (const auto& one : cases) {
    parsev1::Target target;
    *target.mutable_repository() = sample_target();
    one.edit(target.mutable_repository());
    const grpc::Status status = storage::check_target_allowed(target);
    require_equal(static_cast<int>(status.error_code()),
                  static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT),
                  one.path + " is refused as the caller's to fix");
    const parsev1::OptionRejection rejection = rejection_of(status);
    require_equal(rejection.option_path(), one.path, "the refusal names the field");
    require_equal(static_cast<int>(rejection.reason()),
                  static_cast<int>(parsev1::OPTION_REJECTION_REASON_INVALID),
                  one.path + " is an invalid value");
  }
  require_equal(fixture.repository.uploads() + fixture.repository.reads(), 0,
                "a refused target dials nothing");
}

void verify_the_settings_are_read_strictly_and_never_print_the_credential() {
  {
    ScopedEnv address("GRPARSE_REPOSITORY_TARGET", "  ");
    require(!targets::repository_config_from_environment().has_value(),
            "a blank address is no repository");
    require_equal(targets::describe_repository_setting(),
                  "gRParse repository target: not configured",
                  "the startup line of a server with no repository");
  }
  ScopedEnv address("GRPARSE_REPOSITORY_TARGET", "repo.internal:9090");
  ScopedEnv token("GRPARSE_REPOSITORY_API_TOKEN", "s3cr3t-token\n");
  {
    ScopedEnv tls("GRPARSE_REPOSITORY_TLS", "true");
    ScopedEnv timeout("GRPARSE_REPOSITORY_TIMEOUT_SECONDS", "45");
    const auto config = targets::repository_config_from_environment();
    require(config.has_value(), "an address configures the repository");
    require_equal(config->address, "repo.internal:9090", "the address");
    require_equal(config->api_token, "s3cr3t-token", "the credential, without its newline");
    require(config->tls, "TLS is on when asked for");
    require_equal(config->deadline.count(), 45, "the per-call deadline");
    const std::string line = targets::describe_repository_setting();
    require(line.find("repo.internal:9090") != std::string::npos, "the line names the address");
    require(line.find("api token set") != std::string::npos, "the line says a token is set");
    require(line.find("s3cr3t") == std::string::npos, "the line never holds the credential");
  }
  const auto refuses = [](const char* variable, const char* value) {
    ScopedEnv bad(variable, value);
    try {
      targets::repository_config_from_environment();
    } catch (const std::invalid_argument& error) {
      require(std::string(error.what()).find(variable) != std::string::npos,
              "the failure names the variable");
      return;
    }
    throw std::runtime_error(std::string(variable) + "=" + value + " was accepted");
  };
  refuses("GRPARSE_REPOSITORY_TLS", "yes");
  refuses("GRPARSE_REPOSITORY_TIMEOUT_SECONDS", "0");
  refuses("GRPARSE_REPOSITORY_TIMEOUT_SECONDS", "30s");
  refuses("GRPARSE_REPOSITORY_API_TOKEN", "tok\x01" "en");
}

// ---- the save ---------------------------------------------------------------

void verify_a_create_stores_both_blobs_byte_for_byte_in_one_accession() {
  RepositoryFixture fixture;
  const std::string address = fixture.address();
  ScopedEnv configured("GRPARSE_REPOSITORY_TARGET", address.c_str());
  ScopedEnv token("GRPARSE_REPOSITORY_API_TOKEN", kToken);
  parsev1::Target target;
  *target.mutable_repository() = sample_target();
  target.mutable_repository()->set_connector_id("s3-crawler");
  target.mutable_repository()->set_drive("cold");
  const parsev1::ConvertDocumentResponse converted = sample_conversion();

  parsev1::TargetResult result;
  const grpc::Status status = targets::deliver(target, converted, &result);
  require(status.ok(), "the save commits: " + status.error_message());

  // What the bundle holds is what the repository holds.
  parsev1::TargetResult bundle_result;
  const auto files = targets::build_bundle(converted, &bundle_result);
  const auto history = fixture.repository.revisions(sample_address());
  require_equal(history.size(), size_t{1}, "one accession, one revision");
  const auto& revision = history.front();
  require_equal(revision.blobs.size(), size_t{2}, "the revision holds the two blobs");
  const auto& document_blob = blob(revision, "grparse:document.pb");
  const auto& response_blob = blob(revision, "grparse:response.pb");
  require(document_blob.bytes == member(files, "document.pb"),
          "the stored document.pb is the bundle's, byte for byte");
  require(response_blob.bytes == member(files, "response.pb"),
          "the stored response.pb is the bundle's, byte for byte");
  docv1::Document stored_document;
  require(stored_document.ParseFromString(document_blob.bytes), "the stored blob is a Document");
  require(stored_document.SerializeAsString() == converted.document().doc().SerializeAsString(),
          "the stored Document is the converted one");
  parsev1::ConvertDocumentResponse stored_response;
  require(stored_response.ParseFromString(response_blob.bytes),
          "the stored response.pb is a ConvertDocumentResponse");
  require_equal(static_cast<int>(stored_response.status()),
                static_cast<int>(parsev1::CONVERSION_STATUS_PARTIAL_SUCCESS),
                "a partial conversion reads as partial in the repository");
  require_equal(stored_response.errors_size(), 1, "the conversion's errors are stored with it");

  // What each blob says about itself.
  require_equal(document_blob.blob.filename(), "document.pb", "the blob's filename");
  require_equal(document_blob.blob.mime_type(), "application/x-protobuf", "the media type");
  require_equal(document_blob.blob.schema_subject(), "ai/pipestream/document/v1/document.proto",
                "the Document's schema subject");
  require_equal(response_blob.blob.schema_subject(), "ai/pipestream/parse/v1/parse_types.proto",
                "the response's schema subject");
  require_equal(document_blob.blob.checksum(), targets::sha256_hex(document_blob.bytes),
                "the declared SHA-256");
  require_equal(document_blob.blob.size_bytes(), static_cast<int64_t>(document_blob.bytes.size()),
                "the declared length");

  // The document's identity, and who wrote it.
  require_equal(revision.core.doc_id(), "doc-1", "the document id");
  require_equal(revision.core.ownership().account_id(), "acme", "the owning account");
  require_equal(revision.core.ownership().datasource_id(), "contracts", "the datasource");
  require_equal(revision.core.ownership().connector_id(), "s3-crawler", "the connector");
  require_equal(revision.owner.intake().datasource_id(), "contracts", "the intake writer");
  require_equal(revision.owner.intake().connector_id(), "s3-crawler", "the writer's connector");
  require_equal(revision.drive, "cold", "the drive the request named");

  // How it travelled: one call, the Document in data frames, a declared
  // length and digest so a replay needs no bytes.
  require_equal(fixture.repository.uploads(), 1, "one upload call");
  require_equal(fixture.repository.reads(), 0, "a create reads nothing first");
  require(fixture.repository.data_frames() >= 3,
          "a Document of this size crosses several data frames");
  const auto header = fixture.repository.headers().front();
  require_equal(header.blob_id(), "grparse:document.pb", "the streamed blob");
  require_equal(header.declared_length(), static_cast<int64_t>(document_blob.bytes.size()),
                "the header declares the streamed length");
  require_equal(header.expected_sha256(), targets::sha256_hex(document_blob.bytes),
                "the header declares the streamed digest");
  require(header.save().expected().none(), "a create expects no document");

  // What the caller is told.
  require(result.has_repository(), "the result carries the accession");
  require(!result.has_repository_failure(), "a committed save reports no failure");
  require_equal(result.repository().doc_id(), "doc-1", "the saved address: doc_id");
  require_equal(result.repository().graph_address_id(), "contracts", "graph_address_id");
  require_equal(result.repository().account_id(), "acme", "account_id");
  require_equal(result.repository().graph_id(), "intake:acme", "graph_id");
  require_equal(result.repository().revision(), int64_t{1}, "the committed revision");
  require(!result.repository().has_previous_revision(), "a create replaced nothing");
  require(!result.repository().replayed(), "a first save is not a replay");
  require(result.repository().has_committed_at(), "the commit time is passed on");
  require(result.repository().operation_id().starts_with("grparse-"),
          "an unnamed operation gets a derived id");
  require_equal(result.objects_size(), 2, "both blobs are listed");
  require_equal(result.objects(0).key(), "grparse:document.pb", "the first object");
  require_equal(result.objects(0).size_bytes(), document_blob.bytes.size(),
                "the first object's size");
  require_equal(result.objects(1).key(), "grparse:response.pb", "the second object");
  require(!result.has_archive(), "a repository target builds no archive");
}

void verify_the_same_conversion_saved_twice_replays_without_sending_bytes() {
  RepositoryFixture fixture;
  parsev1::TargetResult bundle_result;
  const auto files = targets::build_bundle(sample_conversion(), &bundle_result);
  parsev1::TargetResult first;
  require(targets::deliver_repository(fixture.config(), sample_target(), files, &first).ok(),
          "the first save commits");
  const int frames_after_first = fixture.repository.data_frames();

  parsev1::TargetResult second;
  const grpc::Status status =
      targets::deliver_repository(fixture.config(), sample_target(), files, &second);
  require(status.ok(), "the same save again succeeds: " + status.error_message());
  require(second.repository().replayed(), "the repository's replay is reported as one");
  require_equal(second.repository().revision(), int64_t{1}, "the replay names the same revision");
  require_equal(second.repository().operation_id(), first.repository().operation_id(),
                "the same content derives the same operation id");
  require_equal(fixture.repository.revisions(sample_address()).size(), size_t{1},
                "a replay writes no new revision");
  require_equal(fixture.repository.data_frames(), frames_after_first,
                "a replay sends no bytes");
}

void verify_a_stale_expectation_is_a_typed_conflict_not_a_save() {
  RepositoryFixture fixture;
  parsev1::TargetResult scratch;
  const auto first_files = targets::build_bundle(sample_conversion("first"), &scratch);
  parsev1::TargetResult first;
  require(targets::deliver_repository(fixture.config(), sample_target(), first_files, &first).ok(),
          "the first save commits");

  // A different conversion that still believes the document does not exist.
  const auto other_files = targets::build_bundle(sample_conversion("second"), &scratch);
  parsev1::TargetResult result;
  const grpc::Status status =
      targets::deliver_repository(fixture.config(), sample_target(), other_files, &result);
  require_equal(static_cast<int>(status.error_code()),
                static_cast<int>(grpc::StatusCode::UNAVAILABLE),
                "a refused save is a failed delivery, never the caller's INVALID_ARGUMENT");
  require(status.error_message().find("ABORTED") != std::string::npos,
          "the message carries the repository's own status");
  require(!result.has_repository(), "a refused save reports no accession");
  require_equal(result.objects_size(), 0, "a refused save lists no objects");
  const auto& failure = result.repository_failure();
  require_equal(static_cast<int>(failure.reason()),
                static_cast<int>(parsev1::REPOSITORY_FAILURE_REASON_REVISION_CONFLICT),
                "the failure is a revision conflict");
  require_equal(failure.grpc_status_code(), static_cast<int>(grpc::StatusCode::ABORTED),
                "the repository's status code");
  require_equal(failure.current_revision(), int64_t{1},
                "the conflict carries the document's current revision");
  require(failure.has_current_is_tombstone() && !failure.current_is_tombstone(),
          "and whether it is a tombstone");
  require(!failure.operation_id().empty(), "the failure names the operation for a lookup");
  require_equal(fixture.repository.revisions(sample_address()).size(), size_t{1},
                "nothing was written over the first save");
}

void verify_an_update_replaces_its_own_blobs_and_keeps_everyone_elses() {
  RepositoryFixture fixture;
  fixture.repository.seed(sample_address(), "source-file", "%PDF-1.7 the original");
  parsev1::TargetResult scratch;

  parsev1::RepositoryTarget onto_first = sample_target();
  onto_first.set_revision(1);
  const auto first_files = targets::build_bundle(sample_conversion("first parse"), &scratch);
  parsev1::TargetResult first;
  grpc::Status status =
      targets::deliver_repository(fixture.config(), onto_first, first_files, &first);
  require(status.ok(), "a save onto an existing revision commits: " + status.error_message());
  require_equal(first.repository().revision(), int64_t{2}, "the new revision");
  require_equal(first.repository().previous_revision(), int64_t{1}, "the revision it replaced");
  require_equal(fixture.repository.reads(), 1, "the expected revision's blobs are read once");

  parsev1::RepositoryTarget onto_second = sample_target();
  onto_second.set_revision(2);
  const auto second_files = targets::build_bundle(sample_conversion("second parse"), &scratch);
  parsev1::TargetResult second;
  status = targets::deliver_repository(fixture.config(), onto_second, second_files, &second);
  require(status.ok(), "a re-parse saves over the first: " + status.error_message());
  require_equal(second.repository().revision(), int64_t{3}, "the re-parse's revision");

  const auto history = fixture.repository.revisions(sample_address());
  require_equal(history.size(), size_t{3}, "three revisions in all");
  const auto& latest = history.back();
  require_equal(latest.blobs.size(), size_t{3},
                "the other writer's blob and this target's two, no stale copies");
  require_equal(blob(latest, "source-file").bytes, "%PDF-1.7 the original",
                "the other writer's blob is kept with its bytes");
  require_equal(blob(latest, "source-file").blob.filename(), "source.pdf",
                "and with what it said about itself");
  require(blob(latest, "grparse:document.pb").bytes == member(second_files, "document.pb"),
          "document.pb is the re-parse's");
  require(blob(latest, "grparse:response.pb").bytes == member(second_files, "response.pb"),
          "response.pb is the re-parse's");
  require_equal(latest.core.doc_id(), "doc-1", "the document's identity is carried, not rewritten");
  // An update addresses the document and writes no CORE.
  const auto header = fixture.repository.headers().back();
  require(header.save().target().has_address(), "an update names the full address");
  require_equal(header.save().writes_size(), 1, "an update writes one part");
  require(header.save().document().doc_id().empty() && !header.save().document().has_ownership(),
          "an update sends no CORE content");
}

void verify_an_oversized_response_takes_a_second_accession() {
  RepositoryFixture fixture;
  targets::RepositoryConfig config = fixture.config();
  config.inline_budget_bytes = 16;
  parsev1::TargetResult scratch;
  const auto files = targets::build_bundle(sample_conversion(), &scratch);
  parsev1::RepositoryTarget target = sample_target();
  target.set_operation_id("crawl-42");

  parsev1::TargetResult result;
  const grpc::Status status = targets::deliver_repository(config, target, files, &result);
  require(status.ok(), "the two-accession save commits: " + status.error_message());
  require_equal(fixture.repository.uploads(), 2, "two upload calls");
  const auto headers = fixture.repository.headers();
  require_equal(headers[0].save().operation_id(), "crawl-42", "the request's operation id");
  require_equal(headers[1].save().operation_id(), "crawl-42/response",
                "the second accession's operation id");
  require_equal(headers[1].blob_id(), "grparse:response.pb", "the second call streams response.pb");
  require_equal(headers[1].save().expected().revision(), int64_t{1},
                "the second accession expects the first one's revision");

  const auto history = fixture.repository.revisions(sample_address());
  require_equal(history.size(), size_t{2}, "two revisions");
  require_equal(history[0].blobs.size(), size_t{1}, "the first holds document.pb alone");
  require(blob(history[1], "grparse:document.pb").bytes == member(files, "document.pb"),
          "the second keeps document.pb");
  require(blob(history[1], "grparse:response.pb").bytes == member(files, "response.pb"),
          "and adds response.pb, byte for byte");
  require_equal(result.repository().revision(), int64_t{2},
                "the result names the revision that holds both blobs");
  require_equal(result.repository().operation_id(), "crawl-42", "under the request's id");
  require(!result.repository().has_previous_revision(), "the save as a whole replaced nothing");
  require_equal(result.objects_size(), 2, "both blobs are listed");
}

void verify_a_second_accession_that_fails_says_what_did_commit() {
  RepositoryFixture fixture;
  fixture.repository.refuse_upload(
      2, grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "Save buffer budget exhausted"));
  targets::RepositoryConfig config = fixture.config();
  config.inline_budget_bytes = 16;
  parsev1::TargetResult scratch;
  const auto files = targets::build_bundle(sample_conversion(), &scratch);

  parsev1::TargetResult result;
  const grpc::Status status =
      targets::deliver_repository(config, sample_target(), files, &result);
  require(!status.ok(), "a save whose second accession failed is not a success");
  require(!result.has_repository(), "and reports no finished accession");
  const auto& failure = result.repository_failure();
  require_equal(static_cast<int>(failure.reason()),
                static_cast<int>(parsev1::REPOSITORY_FAILURE_REASON_RESOURCE_EXHAUSTED),
                "the repository's refusal, classified");
  require_equal(failure.message(), "Save buffer budget exhausted",
                "the repository's message, verbatim");
  require_equal(failure.committed_revision(), int64_t{1},
                "the failure says which revision holds document.pb");
  require(failure.operation_id().ends_with("/response"),
          "and names the operation that failed");
  require_equal(result.objects_size(), 1, "the blob that is stored is listed");
  require_equal(result.objects(0).key(), "grparse:document.pb", "it is document.pb");
}

void verify_the_repositorys_refusals_arrive_typed() {
  parsev1::TargetResult scratch;
  const auto files = targets::build_bundle(sample_conversion(), &scratch);
  {
    // A credential the repository does not know.
    RepositoryFixture fixture;
    targets::RepositoryConfig config = fixture.config();
    config.api_token = "not-the-token";
    parsev1::TargetResult result;
    const grpc::Status status =
        targets::deliver_repository(config, sample_target(), files, &result);
    require(!status.ok(), "a refused credential is not a save");
    require_equal(static_cast<int>(result.repository_failure().reason()),
                  static_cast<int>(parsev1::REPOSITORY_FAILURE_REASON_NOT_AUTHORIZED),
                  "a refused credential is NOT_AUTHORIZED");
    require_equal(result.repository_failure().grpc_status_code(),
                  static_cast<int>(grpc::StatusCode::UNAUTHENTICATED), "the repository's code");
    require(status.error_message().find("not-the-token") == std::string::npos,
            "the credential is in no message");
    require_equal(fixture.repository.revisions(sample_address()).size(), size_t{0},
                  "nothing was written");
  }
  {
    // No credential at all.
    RepositoryFixture fixture;
    targets::RepositoryConfig config = fixture.config();
    config.api_token.clear();
    parsev1::TargetResult result;
    require(!targets::deliver_repository(config, sample_target(), files, &result).ok(),
            "a missing credential is not a save");
    require_equal(static_cast<int>(result.repository_failure().reason()),
                  static_cast<int>(parsev1::REPOSITORY_FAILURE_REASON_NOT_AUTHORIZED),
                  "a missing credential is NOT_AUTHORIZED");
  }
  {
    // A rejection with the repository's own reason.
    RepositoryFixture fixture;
    repov2::SaveRejection rejection;
    rejection.set_reason(repov2::REJECTION_REASON_OWNER_UNKNOWN);
    rejection.set_message("No writer grant exists for the owner");
    fixture.repository.refuse_upload(
        1, status_with(grpc::StatusCode::FAILED_PRECONDITION, rejection.message(), rejection));
    parsev1::TargetResult result;
    require(!targets::deliver_repository(fixture.config(), sample_target(), files, &result).ok(),
            "a rejected save is not a save");
    const auto& failure = result.repository_failure();
    require_equal(static_cast<int>(failure.reason()),
                  static_cast<int>(parsev1::REPOSITORY_FAILURE_REASON_REJECTED),
                  "a rejection is REJECTED");
    require_equal(failure.rejection_raw(), "REJECTION_REASON_OWNER_UNKNOWN",
                  "with the repository's own reason by name");
    require_equal(failure.message(), "No writer grant exists for the owner",
                  "and its message verbatim");
  }
  {
    // An operation id already used for something else.
    RepositoryFixture fixture;
    parsev1::RepositoryTarget target = sample_target();
    target.set_operation_id("fixed-id");
    parsev1::TargetResult first;
    require(targets::deliver_repository(fixture.config(), target, files, &first).ok(),
            "the first save under the id commits");
    const auto other = targets::build_bundle(sample_conversion("other"), &scratch);
    parsev1::TargetResult result;
    require(!targets::deliver_repository(fixture.config(), target, other, &result).ok(),
            "the id cannot be reused for other content");
    require_equal(static_cast<int>(result.repository_failure().reason()),
                  static_cast<int>(parsev1::REPOSITORY_FAILURE_REASON_OPERATION_REUSED),
                  "a reused id is OPERATION_REUSED");
    require_equal(result.repository_failure().operation_id(), "fixed-id", "the id in question");
  }
  {
    // An expected revision that is not there.
    RepositoryFixture fixture;
    parsev1::RepositoryTarget target = sample_target();
    target.set_revision(7);
    parsev1::TargetResult result;
    require(!targets::deliver_repository(fixture.config(), target, files, &result).ok(),
            "a revision that does not exist is not saved onto");
    require_equal(static_cast<int>(result.repository_failure().reason()),
                  static_cast<int>(parsev1::REPOSITORY_FAILURE_REASON_NOT_FOUND),
                  "a missing revision is NOT_FOUND");
    require_equal(fixture.repository.uploads(), 0, "and nothing is uploaded");
  }
}

void verify_an_answer_that_names_another_operation_is_not_believed() {
  RepositoryFixture fixture;
  fixture.repository.answer_another_operation();
  parsev1::TargetResult scratch;
  const auto files = targets::build_bundle(sample_conversion(), &scratch);
  parsev1::TargetResult result;
  const grpc::Status status =
      targets::deliver_repository(fixture.config(), sample_target(), files, &result);
  require(!status.ok(), "a commit that names another operation is not reported as this save");
  require(!result.has_repository(), "no accession is passed on");
  require_equal(static_cast<int>(result.repository_failure().reason()),
                static_cast<int>(parsev1::REPOSITORY_FAILURE_REASON_PROTOCOL),
                "the failure is a protocol break");
  require_equal(result.repository_failure().grpc_status_code(), 0,
                "on a call that itself ended OK");
}

void verify_an_unreachable_repository_fails_the_delivery_loudly() {
  targets::RepositoryConfig config;
  // Nothing listens on the discard port of loopback.
  config.address = "127.0.0.1:9";
  config.api_token = kToken;
  config.deadline = std::chrono::seconds(5);
  parsev1::TargetResult scratch;
  const auto files = targets::build_bundle(sample_conversion(), &scratch);
  parsev1::TargetResult result;
  const grpc::Status status =
      targets::deliver_repository(config, sample_target(), files, &result);
  require_equal(static_cast<int>(status.error_code()),
                static_cast<int>(grpc::StatusCode::UNAVAILABLE),
                "an unreachable repository fails the delivery");
  require(!result.has_repository(), "and reports no accession");
  const int reason = static_cast<int>(result.repository_failure().reason());
  require(reason == static_cast<int>(parsev1::REPOSITORY_FAILURE_REASON_UNREACHABLE) ||
              reason == static_cast<int>(parsev1::REPOSITORY_FAILURE_REASON_DEADLINE_EXCEEDED),
          "as unreachable (or out of time), typed");
  require(!result.repository_failure().operation_id().empty(),
          "with the operation id an outcome lookup needs");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("repository-target-test", "ok", {
      verify_a_server_with_no_repository_refuses_the_target_by_name,
      verify_a_listed_target_policy_must_name_the_repository,
      verify_an_incomplete_target_is_refused_field_by_field,
      verify_the_settings_are_read_strictly_and_never_print_the_credential,
      verify_a_create_stores_both_blobs_byte_for_byte_in_one_accession,
      verify_the_same_conversion_saved_twice_replays_without_sending_bytes,
      verify_a_stale_expectation_is_a_typed_conflict_not_a_save,
      verify_an_update_replaces_its_own_blobs_and_keeps_everyone_elses,
      verify_an_oversized_response_takes_a_second_accession,
      verify_a_second_accession_that_fails_says_what_did_commit,
      verify_the_repositorys_refusals_arrive_typed,
      verify_an_answer_that_names_another_operation_is_not_believed,
      verify_an_unreachable_repository_fails_the_delivery_loudly,
  });
}
