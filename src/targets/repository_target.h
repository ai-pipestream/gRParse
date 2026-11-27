// The repository target: a conversion saved in a document repository over
// gRPC, through the parts-based ai.protomolt.proto.repo.v2.DocumentService
// (contract vendored under repository/proto, pinned in collectors/PINS.tsv).
//
// It is an add-on. Nothing here runs, and no channel is opened, until a
// request names RepositoryTarget; a server with no repository configured
// builds, starts and parses exactly as before and refuses the target by
// name. The deployment says where the repository is and how the server
// authenticates to it; the request says which document the result is.
//
// Settings, read from the environment:
//   GRPARSE_REPOSITORY_TARGET           host:port of the repository's gRPC
//                                       listener. Unset or blank: no
//                                       repository, and the target is
//                                       refused.
//   GRPARSE_REPOSITORY_API_TOKEN        the credential sent as api_token
//                                       call metadata. Unset sends none.
//   GRPARSE_REPOSITORY_TLS              "1" or "true" dials with TLS against
//                                       the system roots; "0", "false" or
//                                       unset dials plaintext.
//   GRPARSE_REPOSITORY_TIMEOUT_SECONDS  the deadline of each repository
//                                       call, 1 to 86400; default 300.
// A value outside those is a startup failure, not a default.
#ifndef GRPARSE_TARGETS_REPOSITORY_TARGET_H
#define GRPARSE_TARGETS_REPOSITORY_TARGET_H

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "bundle.h"

namespace grparse::targets {

// What response.pb may weigh and still ride the call that streams the
// parser result. The repository bounds one inbound message (10 MiB on its
// stock listener) and the same frame carries the save itself, so the budget
// stays well under it.
inline constexpr size_t kRepositoryInlineBudgetBytes = 4 * 1024 * 1024;

struct RepositoryConfig {
  std::string address;
  // Empty sends no credential.
  std::string api_token;
  bool tls = false;
  std::chrono::seconds deadline{300};
  // kRepositoryInlineBudgetBytes outside tests.
  size_t inline_budget_bytes = kRepositoryInlineBudgetBytes;
};

// The deployment's repository, or nullopt when GRPARSE_REPOSITORY_TARGET is
// unset or blank. std::invalid_argument naming the variable for a value the
// table above does not allow.
std::optional<RepositoryConfig> repository_config_from_environment();

// One line for the startup log saying whether a repository is configured
// and how it is dialled; the credential itself is never part of it. Throws
// what repository_config_from_environment throws, so a bad setting stops
// the server at startup instead of failing the first save.
std::string describe_repository_setting();

// Everything about a RepositoryTarget that can be refused before the
// conversion runs: a server with no repository configured, and a target
// whose own fields are missing or out of bounds. INVALID_ARGUMENT with a
// typed OptionRejection naming the field.
grpc::Status check_repository_target(const ai::pipestream::parse::v1::RepositoryTarget& target);

// Saves the bundle's document.pb as the typed parser result and its
// response.pb as the blob RepositoryTarget documents, and fills `result`:
// `repository` and `objects` when the repository committed,
// `repository_failure` when it did not.
// Blocking. A save the repository refused or that broke on the way returns
// UNAVAILABLE with the repository's own status in the message and in
// result->repository_failure; it never returns OK for a save that did not
// commit, and never INVALID_ARGUMENT, which would cost the caller the
// conversion.
grpc::Status deliver_repository(const RepositoryConfig& config,
                                const ai::pipestream::parse::v1::RepositoryTarget& target,
                                const std::vector<BundleFile>& files,
                                ai::pipestream::parse::v1::TargetResult* result);

}  // namespace grparse::targets

#endif
