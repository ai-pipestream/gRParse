// A unary surface returns the whole conversion in one message, and nothing
// used to cap it: a 20 MB CSV came back as a 770 MB response no client could
// receive. The cap refuses such a response with RESOURCE_EXHAUSTED once the
// parse is whole, naming both sizes and the streaming surface; a response
// that fits passes untouched, and the setting that sizes the cap reads like
// every other byte budget.
#include <cstdlib>
#include <stdexcept>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse.pb.h"
#include "grparse/document_parser_service.h"
#include "server_config.h"
#include "support/check.h"

namespace {

namespace parsev1 = ai::pipestream::parse::v1;

using grparse_test::require;

// A response carrying `bytes` of markdown export, so its size is known.
parsev1::ConvertSourceResponse response_of(size_t bytes) {
  parsev1::ConvertSourceResponse response;
  auto* exports = response.mutable_response()->mutable_document()->mutable_exports();
  exports->set_md(std::string(bytes, 'm'));
  return response;
}

void verify_a_response_that_fits_passes() {
  const auto response = response_of(1000);
  const grpc::Status status =
      grparse::refuse_oversized_response(response, response.ByteSizeLong(), "ConvertSource");
  require(status.ok(), "a response exactly at the cap passes: " + status.error_message());
  require(grparse::refuse_oversized_response(response, 1 << 20, "ConvertSource").ok(),
          "a response under the cap passes");
}

void verify_an_oversized_response_is_refused_with_both_sizes() {
  const auto response = response_of(100000);
  const grpc::Status status = grparse::refuse_oversized_response(response, 65536, "ConvertSource");
  require(status.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED,
          "a response over the cap is RESOURCE_EXHAUSTED, not " +
              std::to_string(status.error_code()));
  const std::string& message = status.error_message();
  require(message.starts_with("ConvertSource: "), "the surface names itself: " + message);
  require(message.contains(std::to_string(response.ByteSizeLong()) + " bytes"),
          "the message carries the response's size: " + message);
  require(message.contains("65536 byte"), "the message carries the cap: " + message);
  require(message.contains("GRPARSE_MAX_RESPONSE_BYTES"),
          "the message names the setting: " + message);
  require(message.contains("StreamProcessDocument"),
          "the message points at the streaming surface: " + message);
}

void verify_the_cap_setting_defaults_to_the_servers_own_limit() {
  ::unsetenv("GRPARSE_MAX_RESPONSE_BYTES");
  require(grparse::read_response_byte_cap() == static_cast<uint64_t>(grparse::kMaxMessageBytes),
          "unset, the cap is the size the server accepts itself");
  ::setenv("GRPARSE_MAX_RESPONSE_BYTES", "4194304", 1);
  require(grparse::read_response_byte_cap() == 4194304, "the setting sizes the cap");
  ::setenv("GRPARSE_MAX_RESPONSE_BYTES", "0", 1);
  bool refused = false;
  try {
    grparse::read_response_byte_cap();
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  require(refused, "a cap of zero is refused at startup");
  ::unsetenv("GRPARSE_MAX_RESPONSE_BYTES");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("unary-response-cap-test", "all checks passed", {
      verify_a_response_that_fits_passes,
      verify_an_oversized_response_is_refused_with_both_sizes,
      verify_the_cap_setting_defaults_to_the_servers_own_limit,
  });
}
