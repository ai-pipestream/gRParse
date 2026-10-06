#include "grparse/document_passwords.h"

#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>

namespace grparse {

grpc::Status read_document_passwords(const grpc::ServerContextBase& context,
                                     DocumentPasswords* passwords) {
  passwords->candidates.clear();
  const auto& metadata = context.client_metadata();
  for (const std::string_view key : {kDocumentPasswordKey, kDocumentPasswordBinKey}) {
    const auto [first, last] = metadata.equal_range(grpc::string_ref(key.data(), key.size()));
    for (auto entry = first; entry != last; ++entry) {
      const grpc::string_ref& value = entry->second;
      if (value.empty()) continue;
      if (value.size() > kMaxDocumentPasswordBytes) {
        passwords->candidates.clear();
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "a " + std::string(key) + " metadata value is longer than " +
                                std::to_string(kMaxDocumentPasswordBytes) + " bytes");
      }
      if (passwords->candidates.size() == kMaxDocumentPasswords) {
        passwords->candidates.clear();
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "more than " + std::to_string(kMaxDocumentPasswords) +
                                " document passwords in the call metadata");
      }
      passwords->candidates.emplace_back(value.data(), value.size());
    }
  }
  return grpc::Status::OK;
}

void attach_document_passwords(grpc::ClientContext& context, const DocumentPasswords& passwords) {
  // The binary key carries any byte sequence and keeps the order the
  // candidates were read in, whichever key each arrived on.
  for (const std::string& candidate : passwords.candidates) {
    context.AddMetadata(std::string(kDocumentPasswordBinKey), candidate);
  }
}

std::string password_attempt_clause(const DocumentPasswords& passwords) {
  if (passwords.empty()) return "no password was supplied";
  if (passwords.size() == 1) return "the supplied password did not open it";
  return "none of the " + std::to_string(passwords.size()) + " supplied passwords opened it";
}

}  // namespace grparse
