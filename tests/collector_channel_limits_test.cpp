// S3 eval finding: a 15 MB HTML page's markup answer came back as
// RESOURCE_EXHAUSTED ("Received message larger than max") because the
// collector channels kept gRPC's 4 MB default while the server itself
// accepts 520 MB. Every collector channel carries the server's limit in
// both directions.
//
// Issue #250: every collector channel also carries keepalive (a peer that
// went silent fails the leg instead of holding it to its deadline), and a
// collector is dialed over a pool of GRPARSE_COLLECTOR_CHANNELS channels,
// each its own HTTP/2 connection, so a peer's SETTINGS_MAX_CONCURRENT_STREAMS
// is not the coordinator's ceiling on that peer.

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <print>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/channel_arguments.h>

#include "ai/pipestream/xml/v1/xml_service.grpc.pb.h"
#include "grparse/document_parser_service.h"
#include "support/check.h"

namespace {

using grparse_test::require;
using grparse_test::require_equal;

namespace parsev1 = ai::pipestream::parse::v1;
namespace xmlv1 = ai::pipestream::xml::v1;

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

// The default keepalive: a ping after 330 s without a frame while a call is
// open (above every default server ping policy), the connection closed when
// no ack comes within 20 s, no ping
// on an idle connection, no cap on pings between data frames, and
// grpc-core's own retries off.
void verify_keepalive_arguments() {
  const grpc::ChannelArguments arguments = grparse::collector_channel_arguments();
  require_equal(integer_argument(arguments, GRPC_ARG_KEEPALIVE_TIME_MS), 330000,
                "collector channels ping after 330 s of silence");
  require_equal(integer_argument(arguments, GRPC_ARG_KEEPALIVE_TIMEOUT_MS), 20000,
                "an unanswered ping closes the connection after 20 s");
  require_equal(integer_argument(arguments, grparse::kPingTimeoutArgument), 20000,
                "grpc-core's ping timeout, which enforces the keepalive timeout, is 20 s too");
  require_equal(integer_argument(arguments, GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS), 0,
                "collector channels do not ping an idle connection");
  require_equal(integer_argument(arguments, GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA), 0,
                "pings between data frames are not capped");
  require_equal(integer_argument(arguments, GRPC_ARG_ENABLE_RETRIES), 0,
                "grpc-core's own retries are off");
  require_equal(integer_argument(arguments, grparse::kChannelIndexArgument), 0,
                "a lone channel is index 0");

  grparse::CollectorChannelOptions shortened;
  shortened.keepalive_time = std::chrono::milliseconds{700};
  shortened.keepalive_timeout = std::chrono::milliseconds{300};
  const grpc::ChannelArguments third = grparse::collector_channel_arguments(shortened, 2);
  require_equal(integer_argument(third, GRPC_ARG_KEEPALIVE_TIME_MS), 700,
                "the options' keepalive time is the channel's");
  require_equal(integer_argument(third, GRPC_ARG_KEEPALIVE_TIMEOUT_MS), 300,
                "the options' keepalive timeout is the channel's");
  require_equal(integer_argument(third, grparse::kPingTimeoutArgument), 300,
                "the ping timeout follows the keepalive timeout");
  require_equal(integer_argument(third, grparse::kChannelIndexArgument), 2,
                "each pool member carries its own index, so no two share a subchannel");
  require_equal(integer_argument(third, GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH),
                grparse::kMaxMessageBytes, "a pool member keeps the message limit");
}

// The channels `calls` consecutive picks hand out, in order.
template <typename Pick>
std::vector<std::shared_ptr<grpc::Channel>> picks(size_t calls, Pick pick) {
  std::vector<std::shared_ptr<grpc::Channel>> handed;
  for (size_t i = 0; i < calls; ++i) handed.push_back(pick());
  return handed;
}

// `handed` cycles through exactly `size` distinct channels in a fixed order.
void require_round_robin(const std::vector<std::shared_ptr<grpc::Channel>>& handed, size_t size,
                         const std::string& what) {
  const std::set<grpc::Channel*> distinct = [&] {
    std::set<grpc::Channel*> seen;
    for (const auto& channel : handed) seen.insert(channel.get());
    return seen;
  }();
  require_equal(distinct.size(), size, what + ": the pool holds that many distinct channels");
  for (size_t i = 0; i + size < handed.size(); ++i) {
    require(handed[i] == handed[i + size], what + ": the picks go round-robin");
  }
  for (size_t i = 0; i + 1 < handed.size() && size > 1; ++i) {
    require(handed[i] != handed[i + 1], what + ": no channel is handed out twice in a row");
  }
}

void verify_pool_size_is_honoured() {
  grparse::CollectorTargets targets;
  targets.xml = "127.0.0.1:1";
  targets.markup = "127.0.0.1:2";
  targets.derender.target = "127.0.0.1:3";
  targets.vlm.target = "127.0.0.1:4";
  {
    grparse::CollectorEndpoints defaults(targets);
    require_equal(defaults.channels_per_collector(), grparse::kDefaultCollectorChannels,
                  "a pool defaults to two channels");
    require_round_robin(picks(6, [&] { return defaults.channel(parsev1::COLLECTOR_XML); }), 2,
                        "default xml pool");
  }
  targets.channel_options.channels = 3;
  grparse::CollectorEndpoints endpoints(targets);
  require_round_robin(picks(9, [&] { return endpoints.channel(parsev1::COLLECTOR_XML); }), 3,
                      "xml pool of three");
  const auto markup = picks(3, [&] { return endpoints.channel(parsev1::COLLECTOR_MARKUP); });
  require_round_robin(markup, 3, "markup pool of three");
  for (const auto& channel : markup) {
    for (const auto& xml : picks(3, [&] { return endpoints.channel(parsev1::COLLECTOR_XML); })) {
      require(channel != xml, "each collector has its own pool");
    }
  }
  // enrich and vlm-convert are peers, not collectors: one channel each,
  // whatever the collector pools hold.
  require_round_robin(picks(3, [&] { return endpoints.enrich_channel(); }), 1,
                      "the enrich channel");
  require_round_robin(picks(3, [&] { return endpoints.vlm_channel(); }), 1, "the vlm channel");

  targets.channel_options.channels = 1;
  grparse::CollectorEndpoints single(targets);
  require_round_robin(picks(4, [&] { return single.channel(parsev1::COLLECTOR_XML); }), 1,
                      "a pool of one");
  require(single.channel(parsev1::COLLECTOR_EPUB) == nullptr,
          "an unconfigured collector still has no channel");
}

// A collector that holds every stream it is handed until released and
// counts how many it held at once, and from how many connections.
class HoldingXmlService final : public xmlv1::XmlParseService::Service {
 public:
  grpc::Status ParseXml(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<xmlv1::ParseXmlResponse, xmlv1::ParseXmlRequest>*) override {
    std::unique_lock lock(mutex_);
    ++open_;
    peers_.insert(context->peer());
    changed_.notify_all();
    changed_.wait_for(lock, std::chrono::seconds{20},
                      [&] { return released_ || context->IsCancelled(); });
    --open_;
    return grpc::Status::OK;
  }

  // Waits until `count` streams are open at once, or `patience` runs out;
  // the count open at that point.
  size_t wait_for_open(size_t count, std::chrono::milliseconds patience) {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, patience, [&] { return open_ >= count; });
    return open_;
  }
  size_t peers() {
    std::lock_guard lock(mutex_);
    return peers_.size();
  }
  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  size_t open_ = 0;
  std::set<std::string> peers_;
  bool released_ = false;
};

struct StreamCount {
  size_t open = 0;
  size_t connections = 0;
  size_t ok = 0;
};

// Opens `streams` calls at once through `pool_size` channels to a collector
// that admits 100 streams per connection (grpc-java's Netty default), and
// reports how many it saw open together and over how many connections,
// once `expected` were open (or eight seconds passed).
StreamCount open_streams_through_pool(size_t pool_size, size_t streams, size_t expected) {
  HoldingXmlService service;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS, 100);
  // grpc-java's Netty frees a stream's slot once both ends have closed it.
  // grpc-core's overload protection instead counts a stream until the
  // server application has released it, and refuses (REFUSED_STREAM) a new
  // stream that arrives past the limit before then. On a loaded runner the
  // queued streams of the single-connection case then race the 100 being
  // torn down and some fail UNAVAILABLE (#281), which is grpc-core server
  // behavior, not the client pool under test. Turning it off makes this
  // fake count streams the way the Netty collectors it stands for do. The
  // key is GRPC_ARG_MAX_CONCURRENT_STREAMS_OVERLOAD_PROTECTION, which
  // grpc-core defines only in an internal header.
  builder.AddChannelArgument("grpc.http.overload_protection", 0);
  builder.RegisterService(&service);
  const auto server = builder.BuildAndStart();
  require(server != nullptr && port != 0, "the fake collector listens");

  grparse::CollectorTargets targets;
  targets.xml = "127.0.0.1:" + std::to_string(port);
  targets.channel_options.channels = pool_size;
  grparse::CollectorEndpoints endpoints(targets);

  std::mutex ok_mutex;
  size_t ok = 0;
  std::vector<std::thread> clients;
  clients.reserve(streams);
  for (size_t i = 0; i < streams; ++i) {
    clients.emplace_back([&] {
      auto stub = xmlv1::XmlParseService::NewStub(endpoints.channel(parsev1::COLLECTOR_XML));
      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{30});
      auto stream = stub->ParseXml(&context);
      stream->WritesDone();
      xmlv1::ParseXmlResponse event;
      while (stream->Read(&event)) {
      }
      if (stream->Finish().ok()) {
        std::lock_guard lock(ok_mutex);
        ++ok;
      }
    });
  }
  StreamCount count;
  service.wait_for_open(expected, std::chrono::seconds{8});
  // A moment more, so a count short of every stream is the connections'
  // ceiling rather than a slow scheduler's snapshot.
  std::this_thread::sleep_for(std::chrono::milliseconds{500});
  count.open = service.wait_for_open(streams, std::chrono::milliseconds{0});
  count.connections = service.peers();
  service.release();
  for (auto& client : clients) client.join();
  count.ok = ok;
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds{5});
  return count;
}

// 150 concurrent streams to a collector that admits 100 per connection all
// start with the default pool of two; one channel starts 100 and queues 50.
void verify_pool_spreads_streams_past_the_peer_stream_limit() {
  const StreamCount pooled =
      open_streams_through_pool(grparse::kDefaultCollectorChannels, 150, 150);
  require_equal(pooled.open, size_t{150},
                "with a pool of two, all 150 streams are open at once");
  require_equal(pooled.connections, size_t{2}, "the pool of two is two connections");
  require_equal(pooled.ok, size_t{150}, "every pooled stream finishes");

  const StreamCount single = open_streams_through_pool(1, 150, 100);
  require_equal(single.open, size_t{100},
                "one connection holds the peer's 100 and queues the other 50");
  require_equal(single.connections, size_t{1}, "a pool of one is one connection");
  require_equal(single.ok, size_t{150}, "the queued streams still finish once released");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("collector_channel_limits_test", "ok", {
      verify_limits,
      verify_keepalive_arguments,
      verify_pool_size_is_honoured,
      verify_pool_spreads_streams_past_the_peer_stream_limit,
  });
}
