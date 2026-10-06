#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace grpc {
class ClientContext;
class ServerContextBase;
class Status;
}  // namespace grpc

namespace grparse {

// Candidate passwords for an encrypted document, as the caller sent them
// with one request. They arrive in the call's initial metadata, never in the
// request payload: a payload is what gets logged, recaptured and replayed,
// while metadata is read once here and goes nowhere else. Nothing on the
// server stores, configures or logs them; a parse tries them in order on a
// document that needs a password and forgets them when the call ends.
//
// The type has no printer on purpose: no message, log line or recapture
// may carry a candidate, only how many there were.
struct DocumentPasswords {
  std::vector<std::string> candidates;

  bool empty() const { return candidates.empty(); }
  size_t size() const { return candidates.size(); }
};

// The metadata keys. A printable-ASCII password rides the plain key; any
// other password (UTF-8, arbitrary bytes) rides the binary key, which gRPC
// base64-encodes on the wire. Each key may repeat, one candidate per entry;
// the plain key's candidates are tried before the binary key's, each in the
// order sent.
inline constexpr std::string_view kDocumentPasswordKey = "document-password";
inline constexpr std::string_view kDocumentPasswordBinKey = "document-password-bin";

// Bounds on what one call may send, so a request cannot turn a password
// prompt into an unbounded loop of document loads.
inline constexpr size_t kMaxDocumentPasswords = 16;
inline constexpr size_t kMaxDocumentPasswordBytes = 1024;

// Reads the candidates off the call's initial metadata. An empty candidate
// is ignored (it is the same as sending none). Too many candidates, or one
// over the length bound, fail INVALID_ARGUMENT naming the bound, never a
// value.
grpc::Status read_document_passwords(const grpc::ServerContextBase& context,
                                     DocumentPasswords* passwords);

// Attaches the candidates to an outgoing collector call under the same keys,
// so a collector that opens the document itself (grpc-libreoffice) gets them
// the same way gRParse did. Call before the RPC starts.
void attach_document_passwords(grpc::ClientContext& context, const DocumentPasswords& passwords);

// The clause an error message carries about the passwords tried: how many,
// never which.
std::string password_attempt_clause(const DocumentPasswords& passwords);

}  // namespace grparse
