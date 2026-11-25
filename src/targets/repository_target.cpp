#include "repository_target.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <grpc/impl/channel_arg_names.h>

#include "../option_rejection.h"
#include "ai/protomolt/proto/repo/v1/address.pb.h"
#include "ai/protomolt/proto/repo/v1/document.pb.h"
#include "ai/protomolt/proto/repo/v2/document_service.grpc.pb.h"
#include "ai/protomolt/proto/repo/v2/errors.pb.h"
#include "ai/protomolt/proto/repo/v2/manifest.pb.h"
#include "google/rpc/status.pb.h"
#include "sha256.h"

namespace parsev1 = ai::pipestream::parse::v1;
namespace repov1 = ai::protomolt::proto::repo::v1;
namespace repov2 = ai::protomolt::proto::repo::v2;

namespace grparse::targets {
namespace {

// Every blob this target writes carries this prefix, which is how a later
// save tells its own earlier blobs from everybody else's.
constexpr std::string_view kBlobPrefix = "grparse:";
constexpr std::string_view kDocumentMember = "document.pb";
constexpr std::string_view kResponseMember = "response.pb";
constexpr std::string_view kDocumentSubject = "ai/pipestream/document/v1/document.proto";
constexpr std::string_view kResponseSubject = "ai/pipestream/parse/v1/parse_types.proto";
constexpr std::string_view kProtobufMediaType = "application/x-protobuf";
// What the second accession's operation id adds to the first one's.
constexpr std::string_view kResponseOperationSuffix = "/response";
// The repository's own bound on an id (account, datasource, drive,
// operation id), and the operation id bound that leaves room for the suffix.
constexpr size_t kMaxIdChars = 200;
constexpr size_t kMaxOperationIdChars = 180;
constexpr size_t kDataFrameBytes = 256 * 1024;
// A blob bag is metadata only, but a document can hold many blobs.
constexpr int kMaxReceiveBytes = 64 * 1024 * 1024;

std::string trimmed(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return std::string();
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

bool blank(std::string_view text) { return text.find_first_not_of(" \t\r\n") == std::string_view::npos; }

std::string environment(const char* variable) {
  const char* value = std::getenv(variable);
  return value == nullptr ? std::string() : trimmed(value);
}

const char* code_name(grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::OK: return "OK";
    case grpc::StatusCode::CANCELLED: return "CANCELLED";
    case grpc::StatusCode::UNKNOWN: return "UNKNOWN";
    case grpc::StatusCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case grpc::StatusCode::DEADLINE_EXCEEDED: return "DEADLINE_EXCEEDED";
    case grpc::StatusCode::NOT_FOUND: return "NOT_FOUND";
    case grpc::StatusCode::ALREADY_EXISTS: return "ALREADY_EXISTS";
    case grpc::StatusCode::PERMISSION_DENIED: return "PERMISSION_DENIED";
    case grpc::StatusCode::RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
    case grpc::StatusCode::FAILED_PRECONDITION: return "FAILED_PRECONDITION";
    case grpc::StatusCode::ABORTED: return "ABORTED";
    case grpc::StatusCode::OUT_OF_RANGE: return "OUT_OF_RANGE";
    case grpc::StatusCode::UNIMPLEMENTED: return "UNIMPLEMENTED";
    case grpc::StatusCode::INTERNAL: return "INTERNAL";
    case grpc::StatusCode::UNAVAILABLE: return "UNAVAILABLE";
    case grpc::StatusCode::DATA_LOSS: return "DATA_LOSS";
    case grpc::StatusCode::UNAUTHENTICATED: return "UNAUTHENTICATED";
    default: return "UNKNOWN_CODE";
  }
}

parsev1::RepositoryFailureReason reason_of(grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::UNAVAILABLE:
      return parsev1::REPOSITORY_FAILURE_REASON_UNREACHABLE;
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return parsev1::REPOSITORY_FAILURE_REASON_DEADLINE_EXCEEDED;
    case grpc::StatusCode::UNAUTHENTICATED:
    case grpc::StatusCode::PERMISSION_DENIED:
      return parsev1::REPOSITORY_FAILURE_REASON_NOT_AUTHORIZED;
    case grpc::StatusCode::ABORTED:
      return parsev1::REPOSITORY_FAILURE_REASON_REVISION_CONFLICT;
    case grpc::StatusCode::ALREADY_EXISTS:
      return parsev1::REPOSITORY_FAILURE_REASON_OPERATION_REUSED;
    case grpc::StatusCode::INVALID_ARGUMENT:
    case grpc::StatusCode::FAILED_PRECONDITION:
      return parsev1::REPOSITORY_FAILURE_REASON_REJECTED;
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      return parsev1::REPOSITORY_FAILURE_REASON_RESOURCE_EXHAUSTED;
    case grpc::StatusCode::NOT_FOUND:
      return parsev1::REPOSITORY_FAILURE_REASON_NOT_FOUND;
    default:
      return parsev1::REPOSITORY_FAILURE_REASON_OTHER;
  }
}

// The typed details the repository packs into its status: the current
// revision behind a conflict and its own reason for a rejection.
void read_details(const grpc::Status& status, parsev1::RepositoryFailure* failure) {
  if (status.error_details().empty()) return;
  google::rpc::Status rich;
  if (!rich.ParseFromString(status.error_details())) return;
  for (const auto& detail : rich.details()) {
    if (detail.Is<repov2::RevisionConflict>()) {
      repov2::RevisionConflict conflict;
      if (!detail.UnpackTo(&conflict)) continue;
      failure->set_current_revision(conflict.current_revision());
      failure->set_current_is_tombstone(conflict.current_is_tombstone());
    } else if (detail.Is<repov2::SaveRejection>()) {
      repov2::SaveRejection rejection;
      if (!detail.UnpackTo(&rejection)) continue;
      // A reason newer than the vendored contract has no name here; its
      // number is still what the repository said.
      const std::string& name = repov2::RejectionReason_Name(rejection.reason());
      failure->set_rejection_raw(name.empty()
                                     ? std::to_string(static_cast<int>(rejection.reason()))
                                     : name);
    }
  }
}

// A save that did not commit: the typed account on the result, and the
// UNAVAILABLE the target step turns into a partial success.
grpc::Status failed(parsev1::TargetResult* result, parsev1::RepositoryFailureReason reason,
                    const grpc::Status& status, const std::string& operation_id,
                    const std::string& step) {
  auto* failure = result->mutable_repository_failure();
  failure->set_reason(reason);
  failure->set_grpc_status_code(static_cast<int>(status.error_code()));
  failure->set_message(status.error_message());
  failure->set_operation_id(operation_id);
  read_details(status, failure);
  result->clear_repository();
  return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                      "repository target: " + step + " did not commit (" +
                          code_name(status.error_code()) + "): " + status.error_message());
}

grpc::Status failed(parsev1::TargetResult* result, const grpc::Status& status,
                    const std::string& operation_id, const std::string& step) {
  return failed(result, reason_of(status.error_code()), status, operation_id, step);
}

// One channel per repository, opened by the first save that needs it and
// kept: a channel is where the connection and its HTTP/2 session live.
std::shared_ptr<grpc::Channel> channel_for(const RepositoryConfig& config) {
  static std::mutex mutex;
  static std::map<std::pair<std::string, bool>, std::shared_ptr<grpc::Channel>> channels;
  std::lock_guard lock(mutex);
  auto& channel = channels[{config.address, config.tls}];
  if (channel == nullptr) {
    grpc::ChannelArguments arguments;
    arguments.SetMaxReceiveMessageSize(kMaxReceiveBytes);
    // A save is retried by its operation id, by the caller, never by the
    // channel replaying a stream behind the caller's back.
    arguments.SetInt(GRPC_ARG_ENABLE_RETRIES, 0);
    channel = grpc::CreateCustomChannel(
        config.address,
        config.tls ? grpc::SslCredentials(grpc::SslCredentialsOptions())
                   : grpc::InsecureChannelCredentials(),
        arguments);
  }
  return channel;
}

void prepare(grpc::ClientContext* context, const RepositoryConfig& config) {
  context->set_deadline(std::chrono::system_clock::now() + config.deadline);
  if (!config.api_token.empty()) context->AddMetadata("api_token", config.api_token);
}

// One blob this save writes: a bundle member under its blob id.
struct Member {
  std::string blob_id;
  const BundleFile* file = nullptr;
  std::string_view schema_subject;
  std::string sha256;
};

Member member_of(const std::vector<BundleFile>& files, std::string_view path,
                 std::string_view schema_subject) {
  const auto found = std::ranges::find_if(
      files, [path](const BundleFile& file) { return file.path == path; });
  if (found == files.end()) {
    throw std::logic_error("the result bundle holds no " + std::string(path));
  }
  return Member{std::string(kBlobPrefix) + std::string(path), &*found, schema_subject,
                sha256_hex(found->bytes)};
}

// The member as the blob bag lists it. `with_bytes` puts the bytes in the
// message; without them the blob is either the one the call streams or one
// the expected revision already holds under the same id.
repov1::Blob blob_of(const Member& member, bool with_bytes) {
  repov1::Blob blob;
  blob.set_blob_id(member.blob_id);
  if (with_bytes) blob.set_data(member.file->bytes);
  blob.set_mime_type(std::string(kProtobufMediaType));
  blob.set_filename(member.file->path);
  blob.set_size_bytes(static_cast<int64_t>(member.file->bytes.size()));
  blob.set_checksum(member.sha256);
  blob.set_checksum_type(repov1::CHECKSUM_TYPE_SHA256);
  blob.set_schema_subject(std::string(member.schema_subject));
  return blob;
}

repov1::NodeAddress intake_address(const parsev1::RepositoryTarget& target) {
  repov1::NodeAddress address;
  address.set_doc_id(target.doc_id());
  address.set_graph_address_id(target.datasource_id());
  address.set_account_id(target.account_id());
  address.set_graph_id("intake:" + target.account_id());
  return address;
}

repov2::PartWrite put(repov2::DocumentPart part) {
  repov2::PartWrite write;
  write.mutable_selector()->set_part(part);
  write.set_action(repov2::WRITE_ACTION_PUT);
  return write;
}

// The operation id of a save the request did not name one for: a pure
// function of where the result goes, what the caller expects there and the
// bytes saved, so the same conversion sent twice replays.
std::string derived_operation_id(const parsev1::RepositoryTarget& target, const Member& document,
                                 const Member& response) {
  Sha256 hash;
  const auto field = [&hash](std::string_view value) {
    hash.update(std::to_string(value.size()));
    hash.update(":");
    hash.update(value);
  };
  field(target.account_id());
  field(target.datasource_id());
  field(target.doc_id());
  field(target.has_connector_id() ? target.connector_id() : std::string_view());
  field(target.has_drive() ? target.drive() : std::string_view());
  field(target.has_revision() ? "revision:" + std::to_string(target.revision())
                              : std::string("create"));
  field(document.sha256);
  field(response.sha256);
  return "grparse-" + hash.hex_digest();
}

// What every accession of the save shares; `document` and `writes` are the
// caller's to fill.
repov2::SaveDocumentRequest save_frame(const parsev1::RepositoryTarget& target,
                                       const std::string& operation_id) {
  repov2::SaveDocumentRequest save;
  save.set_operation_id(operation_id);
  if (target.has_drive()) save.set_drive(target.drive());
  save.mutable_owner()->mutable_intake()->set_datasource_id(target.datasource_id());
  return save;
}

// The header of one streamed upload: `save` with `blobs` as its blob bag and
// `streamed` as the blob the data frames carry.
repov2::UploadHeader header_of(repov2::SaveDocumentRequest save, std::vector<repov1::Blob> blobs,
                               const Member& streamed) {
  auto* bag = save.mutable_document()->mutable_blob_bag()->mutable_blobs();
  for (auto& blob : blobs) *bag->add_blob() = std::move(blob);
  *save.add_writes() = put(repov2::DOCUMENT_PART_BLOBS);
  repov2::UploadHeader header;
  *header.mutable_save() = std::move(save);
  header.set_blob_id(streamed.blob_id);
  header.set_declared_length(static_cast<int64_t>(streamed.file->bytes.size()));
  header.set_expected_sha256(streamed.sha256);
  header.mutable_streamed();
  return header;
}

struct Upload {
  grpc::Status status;
  repov2::SaveDocumentResponse saved;
  // The repository answered with an outcome it had already recorded.
  bool replayed = false;
  // Set when the call ended OK without the answers the contract requires.
  std::string protocol_error;
};

// One UploadDocument call: the header, the repository's answer to it, the
// bytes when it accepted them, and the committed save.
Upload upload(repov2::DocumentService::Stub& stub, const RepositoryConfig& config,
              const repov2::UploadHeader& header, std::string_view body) {
  Upload outcome;
  grpc::ClientContext context;
  prepare(&context, config);
  const auto stream = stub.UploadDocument(&context);
  const auto finish_with = [&](std::string protocol_error) {
    outcome.status = stream->Finish();
    if (outcome.status.ok()) outcome.protocol_error = std::move(protocol_error);
  };
  // A stream the contract has already been broken on is not left to commit
  // whatever it holds: it is cancelled, and reported as the break it was.
  const auto abandon = [&](std::string protocol_error) {
    context.TryCancel();
    stream->Finish();
    outcome.status = grpc::Status::OK;
    outcome.protocol_error = std::move(protocol_error);
  };

  repov2::UploadDocumentRequest frame;
  *frame.mutable_header() = header;
  if (!stream->Write(frame)) {
    finish_with("the repository closed the upload before reading its header");
    return outcome;
  }
  repov2::UploadDocumentResponse answer;
  if (!stream->Read(&answer)) {
    stream->WritesDone();
    finish_with("the repository ended the upload without answering its header");
    return outcome;
  }
  if (answer.answer_case() == repov2::UploadDocumentResponse::kReplayed) {
    outcome.saved = answer.replayed();
    outcome.replayed = true;
    stream->WritesDone();
    finish_with(std::string());
    return outcome;
  }
  if (answer.answer_case() != repov2::UploadDocumentResponse::kAccepted) {
    abandon("the repository answered a streamed upload's header with neither accepted nor "
            "replayed");
    return outcome;
  }
  bool sent = true;
  for (size_t offset = 0; sent && offset < body.size(); offset += kDataFrameBytes) {
    repov2::UploadDocumentRequest data;
    data.set_data(body.substr(offset, kDataFrameBytes));
    sent = stream->Write(data);
  }
  stream->WritesDone();
  if (!stream->Read(&answer)) {
    finish_with(sent ? "the repository ended the upload without a saved answer"
                     : "the repository closed the upload part way through its bytes");
    return outcome;
  }
  if (answer.answer_case() != repov2::UploadDocumentResponse::kSaved) {
    abandon("the repository answered the uploaded bytes with something other than saved");
    return outcome;
  }
  outcome.saved = answer.saved();
  outcome.replayed = outcome.saved.replayed();
  finish_with(std::string());
  return outcome;
}

// A commit is only believed when it names the operation that was sent and
// a revision; anything else is reported, not passed on as a save.
std::string commit_defect(const Upload& outcome, const std::string& operation_id) {
  if (!outcome.protocol_error.empty()) return outcome.protocol_error;
  if (outcome.saved.operation_id() != operation_id) {
    return "the repository's answer names operation '" + outcome.saved.operation_id() +
           "', not the one sent";
  }
  if (!outcome.saved.has_accession() || outcome.saved.accession().revision() <= 0) {
    return "the repository's answer carries no committed revision";
  }
  return std::string();
}

void report_object(const Member& member, parsev1::TargetResult* result) {
  auto* stored = result->add_objects();
  stored->set_key(member.blob_id);
  stored->set_size_bytes(member.file->bytes.size());
}

// The blobs of the expected revision that are not this target's own, as a
// BLOBS PUT keeps them: by id, without bytes, a storage reference or the
// repository's write stamp, none of which a client may send.
grpc::Status read_kept_blobs(repov2::DocumentService::Stub& stub, const RepositoryConfig& config,
                             const parsev1::RepositoryTarget& target,
                             std::vector<repov1::Blob>* kept) {
  repov2::GetDocumentRequest request;
  *request.mutable_address() = intake_address(target);
  request.mutable_revision()->set_revision(target.revision());
  request.mutable_selection()->add_selectors()->set_part(repov2::DOCUMENT_PART_BLOBS);
  repov2::GetDocumentResponse response;
  grpc::ClientContext context;
  prepare(&context, config);
  const grpc::Status status = stub.GetDocument(&context, request, &response);
  if (!status.ok()) return status;
  const auto& bag = response.document().blob_bag();
  const auto keep = [kept](const repov1::Blob& blob) {
    if (blob.blob_id().starts_with(kBlobPrefix)) return;
    repov1::Blob copy = blob;
    copy.clear_content();
    copy.clear_written();
    kept->push_back(std::move(copy));
  };
  if (bag.has_blob()) keep(bag.blob());
  for (const auto& blob : bag.blobs().blob()) keep(blob);
  return grpc::Status::OK;
}

grpc::Status invalid(const std::string& field, const std::string& value,
                     const std::string& message) {
  return option_rejected("target.repository." + field, parsev1::OPTION_REJECTION_REASON_INVALID,
                         value, "target.repository." + field + " " + message);
}

grpc::Status check_id(const std::string& field, const std::string& value, size_t max_chars) {
  if (blank(value)) return invalid(field, value, "is required and must not be blank");
  if (value.size() > max_chars) {
    return invalid(field, value, "is longer than " + std::to_string(max_chars) + " characters");
  }
  return grpc::Status::OK;
}

}  // namespace

std::optional<RepositoryConfig> repository_config_from_environment() {
  RepositoryConfig config;
  config.address = environment("GRPARSE_REPOSITORY_TARGET");
  if (config.address.empty()) return std::nullopt;
  config.api_token = environment("GRPARSE_REPOSITORY_API_TOKEN");
  // Call metadata is visible ASCII; a token that is not would be dropped
  // or mangled on the wire, and the repository would only say "missing".
  if (std::ranges::any_of(config.api_token,
                          [](unsigned char c) { return c < 0x20 || c > 0x7e; })) {
    throw std::invalid_argument(
        "GRPARSE_REPOSITORY_API_TOKEN holds a character call metadata cannot carry");
  }
  const std::string tls = environment("GRPARSE_REPOSITORY_TLS");
  if (tls == "1" || tls == "true") {
    config.tls = true;
  } else if (!tls.empty() && tls != "0" && tls != "false") {
    throw std::invalid_argument("GRPARSE_REPOSITORY_TLS must be 1, true, 0 or false, not '" +
                                tls + "'");
  }
  const std::string timeout = environment("GRPARSE_REPOSITORY_TIMEOUT_SECONDS");
  if (!timeout.empty()) {
    long seconds = 0;
    const auto parsed = std::from_chars(timeout.data(), timeout.data() + timeout.size(), seconds);
    if (parsed.ec != std::errc() || parsed.ptr != timeout.data() + timeout.size() ||
        seconds < 1 || seconds > 86400) {
      throw std::invalid_argument(
          "GRPARSE_REPOSITORY_TIMEOUT_SECONDS must be a whole number from 1 to 86400, not '" +
          timeout + "'");
    }
    config.deadline = std::chrono::seconds(seconds);
  }
  return config;
}

std::string describe_repository_setting() {
  const auto config = repository_config_from_environment();
  if (!config.has_value()) return "gRParse repository target: not configured";
  return "gRParse repository target: " + config->address + " (" +
         (config->tls ? "TLS" : "plaintext") + ", " +
         (config->api_token.empty() ? "no api token" : "api token set") + ", " +
         std::to_string(config->deadline.count()) +
         " s per call; dialled only when a request names it)";
}

grpc::Status check_repository_target(const parsev1::RepositoryTarget& target) {
  std::optional<RepositoryConfig> config;
  try {
    config = repository_config_from_environment();
  } catch (const std::invalid_argument& misconfigured) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, misconfigured.what());
  }
  if (!config.has_value()) {
    return option_rejected("target.repository", parsev1::OPTION_REJECTION_REASON_NOT_ALLOWED,
                           "repository",
                           "target.repository: this server has no document repository "
                           "configured (GRPARSE_REPOSITORY_TARGET)");
  }
  if (grpc::Status id = check_id("account_id", target.account_id(), kMaxIdChars); !id.ok()) {
    return id;
  }
  if (grpc::Status id = check_id("datasource_id", target.datasource_id(), kMaxIdChars);
      !id.ok()) {
    return id;
  }
  if (grpc::Status id = check_id("doc_id", target.doc_id(), kMaxIdChars); !id.ok()) return id;
  if (target.has_connector_id()) {
    if (grpc::Status id = check_id("connector_id", target.connector_id(), kMaxIdChars);
        !id.ok()) {
      return id;
    }
  }
  if (target.has_drive()) {
    if (grpc::Status id = check_id("drive", target.drive(), kMaxIdChars); !id.ok()) return id;
  }
  if (target.has_operation_id()) {
    if (grpc::Status id = check_id("operation_id", target.operation_id(), kMaxOperationIdChars);
        !id.ok()) {
      return id;
    }
  }
  switch (target.expected_case()) {
    case parsev1::RepositoryTarget::kCreate:
      if (!target.create()) {
        return invalid("create", "false",
                       "must be true when set; name the expected revision instead");
      }
      break;
    case parsev1::RepositoryTarget::kRevision:
      if (target.revision() <= 0) {
        return invalid("revision", std::to_string(target.revision()),
                       "must be a revision the repository reported, which is above zero");
      }
      break;
    case parsev1::RepositoryTarget::EXPECTED_NOT_SET:
      return invalid("expected", std::string(),
                     "is required: set create for a new document or revision for an "
                     "existing one");
  }
  return grpc::Status::OK;
}

grpc::Status deliver_repository(const RepositoryConfig& config,
                                const parsev1::RepositoryTarget& target,
                                const std::vector<BundleFile>& files,
                                parsev1::TargetResult* result) {
  result->clear_repository();
  result->clear_repository_failure();
  Member document;
  Member response;
  try {
    document = member_of(files, kDocumentMember, kDocumentSubject);
    response = member_of(files, kResponseMember, kResponseSubject);
  } catch (const std::logic_error& defect) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        std::string("repository target: ") + defect.what());
  }
  const std::string operation_id = target.has_operation_id()
                                       ? target.operation_id()
                                       : derived_operation_id(target, document, response);
  const auto stub = repov2::DocumentService::NewStub(channel_for(config));

  // The blobs the document already holds that are not this target's own
  // stay; a BLOBS PUT that left them out would drop them.
  std::vector<repov1::Blob> kept;
  if (target.has_revision()) {
    if (const grpc::Status read = read_kept_blobs(*stub, config, target, &kept); !read.ok()) {
      return failed(result, read, operation_id, "reading the expected revision's blobs");
    }
  }

  // The first accession: document.pb streamed, response.pb beside it when
  // it fits the frame.
  const bool response_rides = response.file->bytes.size() <= config.inline_budget_bytes;
  repov2::SaveDocumentRequest save = save_frame(target, operation_id);
  if (target.has_revision()) {
    *save.mutable_target()->mutable_address() = intake_address(target);
    save.mutable_expected()->set_revision(target.revision());
  } else {
    // A new intake document: its identity and ownership are CORE content.
    save.mutable_target()->set_use_datasource_id(target.datasource_id());
    save.mutable_expected()->set_none(true);
    save.mutable_document()->set_doc_id(target.doc_id());
    auto* ownership = save.mutable_document()->mutable_ownership();
    ownership->set_account_id(target.account_id());
    ownership->set_datasource_id(target.datasource_id());
    if (target.has_connector_id()) {
      ownership->set_connector_id(target.connector_id());
      save.mutable_owner()->mutable_intake()->set_connector_id(target.connector_id());
    }
    *save.add_writes() = put(repov2::DOCUMENT_PART_CORE);
  }
  std::vector<repov1::Blob> blobs = kept;
  blobs.push_back(blob_of(document, false));
  if (response_rides) blobs.push_back(blob_of(response, true));
  const Upload first =
      upload(*stub, config, header_of(std::move(save), std::move(blobs), document),
             document.file->bytes);
  if (!first.status.ok()) return failed(result, first.status, operation_id, "the save");
  if (const std::string defect = commit_defect(first, operation_id); !defect.empty()) {
    return failed(result, parsev1::REPOSITORY_FAILURE_REASON_PROTOCOL,
                  grpc::Status(grpc::StatusCode::OK, defect), operation_id, "the save");
  }
  report_object(document, result);

  const repov2::Accession* last = &first.saved.accession();
  bool replayed = first.replayed;
  Upload second;
  if (!response_rides) {
    // response.pb is too large for the frame that carried the save, so it
    // is streamed onto the revision that save just committed.
    const std::string second_id = operation_id + std::string(kResponseOperationSuffix);
    repov2::SaveDocumentRequest follow = save_frame(target, second_id);
    *follow.mutable_target()->mutable_address() = first.saved.accession().address();
    follow.mutable_expected()->set_revision(first.saved.accession().revision());
    std::vector<repov1::Blob> follow_blobs = kept;
    follow_blobs.push_back(blob_of(document, false));
    follow_blobs.push_back(blob_of(response, false));
    second = upload(*stub, config, header_of(std::move(follow), std::move(follow_blobs), response),
                    response.file->bytes);
    grpc::Status broken = grpc::Status::OK;
    if (!second.status.ok()) {
      broken = failed(result, second.status, second_id, "the save of response.pb");
    } else if (const std::string defect = commit_defect(second, second_id); !defect.empty()) {
      broken = failed(result, parsev1::REPOSITORY_FAILURE_REASON_PROTOCOL,
                      grpc::Status(grpc::StatusCode::OK, defect), second_id,
                      "the save of response.pb");
    }
    if (!broken.ok()) {
      // document.pb is in the repository all the same, and the caller is
      // told where.
      result->mutable_repository_failure()->set_committed_revision(
          first.saved.accession().revision());
      return broken;
    }
    last = &second.saved.accession();
    replayed = replayed && second.replayed;
  }
  report_object(response, result);

  auto* committed = result->mutable_repository();
  committed->set_doc_id(last->address().doc_id());
  committed->set_graph_address_id(last->address().graph_address_id());
  committed->set_account_id(last->address().account_id());
  committed->set_graph_id(last->address().graph_id());
  committed->set_revision(last->revision());
  if (first.saved.accession().previous_revision() > 0) {
    committed->set_previous_revision(first.saved.accession().previous_revision());
  }
  committed->set_operation_id(operation_id);
  committed->set_replayed(replayed);
  if (last->has_committed_at()) *committed->mutable_committed_at() = last->committed_at();
  return grpc::Status::OK;
}

}  // namespace grparse::targets
