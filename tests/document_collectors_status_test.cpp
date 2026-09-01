// S3 eval finding: a 15 MB HTML page failed its markup leg and the parse
// reported "markup collector: " with nothing after the colon, because the
// collector's status carried a code and no message. The text a failed leg
// reports keeps the code when the message is empty.

#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>

#include <grpcpp/support/status.h>

#include "grparse/document_collectors.h"
#include "support/check.h"

namespace {

using grparse_test::require;

void verify_status_text() {
  const std::string with_message = grparse::collector_status_text(
      "markup", grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "Received message larger than max"));
  require(with_message == "markup collector: Received message larger than max",
          "a message is reported verbatim: " + with_message);
  const std::string bare = grparse::collector_status_text("markup", grpc::Status(grpc::StatusCode::UNAVAILABLE, ""));
  require(bare == "markup collector: status 14 (UNAVAILABLE)", "an empty message names the code: " + bare);
  const std::string unknown = grparse::collector_status_text("xml", grpc::Status(grpc::StatusCode::UNKNOWN, ""));
  require(unknown == "xml collector: status 2 (UNKNOWN)", "every code has its name: " + unknown);
}

}  // namespace

int main() {
  return grparse_test::run_test_main("document_collectors_status_test", "ok", {
      verify_status_text,
  });
}
