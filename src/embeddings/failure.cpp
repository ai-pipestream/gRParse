#include "failure.h"

#include <cstddef>
#include <print>
#include <string>

namespace grparse::embedding::detail {
namespace {

// Longer than any status line a runtime writes, short enough that a runtime
// echoing a whole input batch cannot flood the log.
constexpr std::size_t kMaxDetailBytes = 2048;

// `detail` as one bounded log line: control characters become spaces, and a
// cut lands on a UTF-8 character boundary.
std::string one_line(std::string_view detail) {
  bool cut = false;
  if (detail.size() > kMaxDetailBytes) {
    std::size_t end = kMaxDetailBytes;
    while (end > 0 && (static_cast<unsigned char>(detail[end]) & 0xC0) == 0x80) --end;
    detail = detail.substr(0, end);
    cut = true;
  }
  std::string line(detail);
  for (char& c : line) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) c = ' ';
  }
  if (cut) line += " [cut]";
  return line;
}

}  // namespace

grpc::Status report_failure(std::string_view backend, EmbeddingFailureStage stage,
                            std::string_view detail, const char* client_message) {
  record_embedding_failure(backend, stage);
  // This runs inside catch handlers, on the way to a status. A log line that
  // cannot be written (stderr closed, no memory to format it) must not turn
  // that status into an exception; the count above and the status below
  // still say the batch failed.
  try {
    std::println(stderr, "gRParse embedding failure: backend={} stage={}: {}",
                 backend.empty() ? std::string_view("unknown") : backend,
                 kEmbeddingFailureStages[static_cast<std::size_t>(stage)], one_line(detail));
  } catch (...) {
  }
  return {grpc::StatusCode::INTERNAL, client_message};
}

}  // namespace grparse::embedding::detail
