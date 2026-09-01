// S3 eval finding: a 15 MB HTML page's markup answer came back as
// RESOURCE_EXHAUSTED ("Received message larger than max") because the
// collector channels kept gRPC's 4 MB default while the server itself
// accepts 520 MB. Every collector channel carries the server's limit in
// both directions.

#include <cstdio>
#include <cstring>
#include <print>
#include <stdexcept>
#include <string>

#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/support/channel_arguments.h>

#include "grparse/document_parser_service.h"
#include "support/check.h"

namespace {

using grparse_test::require;

int integer_argument(const grpc::ChannelArguments& arguments, const char* key) {
  const grpc_channel_args args = arguments.c_channel_args();
  for (size_t i = 0; i < args.num_args; i++) {
    if (std::strcmp(args.args[i].key, key) == 0 && args.args[i].type == GRPC_ARG_INTEGER) {
      return args.args[i].value.integer;
    }
  }
  return -2;
}

void verify_limits() {
  const grpc::ChannelArguments arguments = grparse::collector_channel_arguments();
  require(grparse::kMaxMessageBytes > 15 * 1024 * 1024, "the limit clears the page that failed");
  require(integer_argument(arguments, GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH) == grparse::kMaxMessageBytes,
          "collector channels receive up to the server's own limit");
  require(integer_argument(arguments, GRPC_ARG_MAX_SEND_MESSAGE_LENGTH) == grparse::kMaxMessageBytes,
          "collector channels send up to the server's own limit");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("collector_channel_limits_test", "ok", {
      verify_limits,
  });
}
