// What the Health RPC and grpc.health.v1 say about the process (#412). Both
// used to answer "ready" and SERVING whatever had been built or configured;
// these cases stand the unary service up the way main does, with a fake
// recognizer in place of the models, and read both surfaces over the wire.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <format>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/support/byte_buffer.h>

#include "ai/pipestream/parse/v1/parse.grpc.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.grpc.pb.h"
#include "grparse/document_parser_service.h"
#include "grparse/page_scheduler.h"
#include "grparse/remote_page_source.h"
#include "grparse/service_health.h"
#include "grparse/service_version.h"
#include "support/check.h"

namespace {

using namespace std::chrono_literals;
namespace parsev1 = ai::pipestream::parse::v1;

using grparse_test::require;
using grparse_test::require_equal;

// No case here parses a document; the scheduler only needs a recognizer to
// exist.
class IdleRecognizer final : public grparse::PageRecognizer {
 public:
  grparse::OcrPage extract_page(const cv::Mat&) override { return {}; }
};

constexpr grparse::EngineInventory kEveryEngineLoaded{
    .ocr = grparse::EngineState::kLoaded,
    .layout = grparse::EngineState::kLoaded,
    .table_structure = grparse::EngineState::kLoaded,
    .figure_classifier = grparse::EngineState::kLoaded,
    .embeddings = grparse::EngineState::kLoaded,
};

// The unary service on a loopback port with gRPC's default health service
// beside it, wired as main wires it: the health state is handed to the
// service before the port is bound and published right after.
class HealthServer final {
 public:
  explicit HealthServer(grparse::EngineInventory engines, grparse::CollectorTargets targets = {})
      : scheduler_(recognizer_, {}),
        endpoints(std::make_shared<grparse::CollectorEndpoints>(std::move(targets))),
        health(std::make_shared<grparse::ServiceHealth>(engines, endpoints)),
        service_(scheduler_, endpoints),
        streaming_service_(scheduler_, endpoints) {
    service_.set_health(health);
    start();
    publish();
  }

  // A service nobody handed a health state to.
  HealthServer()
      : scheduler_(recognizer_, {}),
        endpoints(std::make_shared<grparse::CollectorEndpoints>(grparse::CollectorTargets{})),
        service_(scheduler_, endpoints),
        streaming_service_(scheduler_, endpoints) {
    start();
  }

  ~HealthServer() {
    server_->Shutdown(std::chrono::system_clock::now() + 2s);
    server_->Wait();
  }

  void publish() const {
    grparse::publish_serving_status(*health, server_->GetHealthCheckService());
  }

  const std::string& address() const { return address_; }

  std::shared_ptr<grpc::Channel> channel() const {
    return grpc::CreateChannel(address_, grpc::InsecureChannelCredentials());
  }

  parsev1::HealthResponse health_rpc() const {
    const auto stub = parsev1::ParseService::NewStub(channel());
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    parsev1::HealthResponse response;
    const grpc::Status status = stub->Health(&context, {}, &response);
    require(status.ok(), "the Health RPC answers OK: " + status.error_message());
    return response;
  }

 private:
  void start() {
    grpc::EnableDefaultHealthCheckService(true);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service_);
    builder.RegisterService(&streaming_service_);
    server_ = builder.BuildAndStart();
    if (!server_ || port == 0) throw std::runtime_error("test server failed to start");
    address_ = "127.0.0.1:" + std::to_string(port);
  }

  IdleRecognizer recognizer_;
  grparse::PageScheduler scheduler_;

 public:
  const std::shared_ptr<grparse::CollectorEndpoints> endpoints;
  const std::shared_ptr<grparse::ServiceHealth> health;

 private:
  grparse::DocumentParserService service_;
  grparse::DocumentStreamingService streaming_service_;
  std::unique_ptr<grpc::Server> server_;
  std::string address_;
};

// grpc.health.v1.Health/Check for `service`, as the name of the
// HealthCheckResponse.ServingStatus it answers, or the gRPC status code's
// number when the call fails (NOT_FOUND for a service the health service was
// never told about). The two messages are one field each, so they are
// spelled on the wire here instead of compiling gRPC's health proto in:
// the request is `string service = 1`, the response `ServingStatus status = 1`.
std::string health_check(const HealthServer& server, const std::string& service) {
  std::string request_bytes;
  if (!service.empty()) {
    require(service.size() < 128, "the service name fits a one-byte length");
    request_bytes.push_back('\x0a');
    request_bytes.push_back(static_cast<char>(service.size()));
    request_bytes += service;
  }
  const grpc::Slice request_slice(request_bytes);
  const grpc::ByteBuffer request(&request_slice, 1);
  grpc::ByteBuffer response;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 5s);
  grpc::GenericStub stub(server.channel());
  std::promise<grpc::Status> done;
  stub.UnaryCall(&context, "/grpc.health.v1.Health/Check", grpc::StubOptions(), &request,
                 &response, [&done](grpc::Status status) { done.set_value(std::move(status)); });
  const grpc::Status status = done.get_future().get();
  if (!status.ok()) return std::format("status {}", static_cast<int>(status.error_code()));
  grpc::Slice answer;
  require(response.TrySingleSlice(&answer).ok() || response.DumpToSingleSlice(&answer).ok(),
          "the health answer is readable");
  // proto3 leaves a zero enum off the wire, so an empty answer is UNKNOWN.
  if (answer.size() == 0) return "UNKNOWN";
  require(answer.size() == 2 && answer.begin()[0] == 0x08,
          "the health answer is one varint in field 1");
  switch (answer.begin()[1]) {
    case 1: return "SERVING";
    case 2: return "NOT_SERVING";
    case 3: return "SERVICE_UNKNOWN";
    default: return std::format("ServingStatus {}", answer.begin()[1]);
  }
}

const parsev1::ComponentHealth& component(const parsev1::HealthResponse& health,
                                          std::string_view name) {
  const parsev1::ComponentHealth* found = nullptr;
  for (const auto& one : health.components()) {
    if (one.name() != name) continue;
    require(found == nullptr, std::format("Health lists {} once", name));
    found = &one;
  }
  require(found != nullptr, std::format("Health lists a component named {}", name));
  return *found;
}

void require_state(const parsev1::HealthResponse& health, parsev1::ServingState state,
                   std::string_view status, std::string_view what) {
  require_equal(parsev1::ServingState_Name(health.state()), parsev1::ServingState_Name(state),
                what);
  require_equal(health.status(), status, std::format("{} (the status text)", what));
}

void require_component(const parsev1::HealthResponse& health, std::string_view name,
                       parsev1::ComponentKind kind, parsev1::ComponentState state) {
  const auto& one = component(health, name);
  require_equal(parsev1::ComponentKind_Name(one.kind()), parsev1::ComponentKind_Name(kind),
                std::format("the kind of {}", name));
  require_equal(parsev1::ComponentState_Name(one.state()), parsev1::ComponentState_Name(state),
                std::format("the state of {}", name));
}

std::string channels_of(const parsev1::ComponentHealth& one) {
  std::string names;
  for (const int channel : one.channels()) {
    names += (names.empty() ? "" : ",") +
             parsev1::ChannelConnectivity_Name(static_cast<parsev1::ChannelConnectivity>(channel));
  }
  return names;
}

// A loopback address nothing listens on: the kernel hands out a free port to
// a socket that is closed again without ever listening.
std::string closed_address() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(fd >= 0, "a socket to find a free port with");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t length = sizeof(address);
  const bool bound = ::bind(fd, reinterpret_cast<const sockaddr*>(&address), length) == 0 &&
                     ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0;
  ::close(fd);
  require(bound, "a free loopback port");
  return "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
}

const std::vector<std::string> kHealthServices{
    "", parsev1::ParseService::service_full_name(),
    parsev1::ParseStreamingService::service_full_name()};

// A bound port is not a started service: until startup is marked complete
// both surfaces say not serving, for the server as a whole and for each
// parsing service by name, and both turn at the same moment.
void verify_health_is_not_serving_until_startup_completes() {
  setenv("GRPARSE_PDF_BACKEND", "pdfium:50069", 1);
  HealthServer server(kEveryEngineLoaded);

  const auto starting = server.health_rpc();
  require_state(starting, parsev1::SERVING_STATE_NOT_SERVING, "starting",
                "Health before startup completes");
  require_equal(starting.version(), grparse::kServiceVersion, "Health still carries the version");
  for (const std::string& service : kHealthServices) {
    require_equal(health_check(server, service), "NOT_SERVING",
                  std::format("grpc.health.v1 for \"{}\" before startup completes", service));
  }

  server.health->mark_startup_complete();
  server.publish();
  require_state(server.health_rpc(), parsev1::SERVING_STATE_SERVING, "ready",
                "Health once startup completes with everything built");
  for (const std::string& service : kHealthServices) {
    require_equal(health_check(server, service), "SERVING",
                  std::format("grpc.health.v1 for \"{}\" once startup completes", service));
  }
}

// A service nobody told what it was started with claims nothing.
void verify_a_service_without_a_health_state_is_not_serving() {
  HealthServer server;
  const auto health = server.health_rpc();
  require_state(health, parsev1::SERVING_STATE_NOT_SERVING, "starting",
                "Health of a service with no health state");
  require_equal(health.components_size(), 0, "it lists no components");
}

// Each engine is reported as startup left it, and one the settings allowed
// but startup did not build makes the whole answer degraded; one the
// operator switched off does not.
void verify_health_names_the_engines_startup_did_not_build() {
  setenv("GRPARSE_PDF_BACKEND", "pdfium:50069", 1);
  HealthServer server(grparse::EngineInventory{
      .ocr = grparse::EngineState::kLoaded,
      .layout = grparse::EngineState::kLoaded,
      .table_structure = grparse::EngineState::kNotLoaded,
      .figure_classifier = grparse::EngineState::kDisabled,
      .embeddings = grparse::EngineState::kDisabled,
  });
  server.health->mark_startup_complete();
  server.publish();

  const auto health = server.health_rpc();
  require_component(health, "ocr", parsev1::COMPONENT_KIND_ENGINE, parsev1::COMPONENT_STATE_LOADED);
  require_component(health, "layout", parsev1::COMPONENT_KIND_ENGINE,
                    parsev1::COMPONENT_STATE_LOADED);
  require_component(health, "table-structure", parsev1::COMPONENT_KIND_ENGINE,
                    parsev1::COMPONENT_STATE_NOT_LOADED);
  require_component(health, "figure-classifier", parsev1::COMPONENT_KIND_ENGINE,
                    parsev1::COMPONENT_STATE_DISABLED);
  require_component(health, "embeddings", parsev1::COMPONENT_KIND_ENGINE,
                    parsev1::COMPONENT_STATE_DISABLED);
  require_state(health, parsev1::SERVING_STATE_SERVING_DEGRADED, "degraded",
                "Health with an engine startup did not build");
  // Degraded still answers requests, so grpc.health.v1 keeps routing here.
  require_equal(health_check(server, ""), "SERVING", "grpc.health.v1 while degraded");

  HealthServer switched_off(grparse::EngineInventory{
      .ocr = grparse::EngineState::kLoaded,
      .layout = grparse::EngineState::kDisabled,
      .table_structure = grparse::EngineState::kDisabled,
      .figure_classifier = grparse::EngineState::kDisabled,
      .embeddings = grparse::EngineState::kDisabled,
  });
  switched_off.health->mark_startup_complete();
  require_state(switched_off.health_rpc(), parsev1::SERVING_STATE_SERVING, "ready",
                "Health with engines the operator switched off");
}

// The PDF backend is read from GRPARSE_PDF_BACKEND as a parse reads it. With
// none named every PDF fails, which is degraded; a named one is listed with
// its target, one component per target of a consensus list, and its channel
// is reported only once a parse has created it.
void verify_health_reports_the_pdf_backend() {
  HealthServer server(kEveryEngineLoaded);
  server.health->mark_startup_complete();

  unsetenv("GRPARSE_PDF_BACKEND");
  const auto none = server.health_rpc();
  require_component(none, "pdf-backend", parsev1::COMPONENT_KIND_PDF_BACKEND,
                    parsev1::COMPONENT_STATE_NOT_CONFIGURED);
  require(!component(none, "pdf-backend").has_target(), "an unset backend has no target");
  require_state(none, parsev1::SERVING_STATE_SERVING_DEGRADED, "degraded",
                "Health with no PDF backend configured");

  // The test server's own port stands in for a backend: any gRPC server
  // completes the connection, which is all a channel state is about.
  setenv("GRPARSE_PDF_BACKEND", server.address().c_str(), 1);
  const auto named = server.health_rpc();
  require_component(named, "pdf-backend", parsev1::COMPONENT_KIND_PDF_BACKEND,
                    parsev1::COMPONENT_STATE_CONFIGURED);
  require_equal(component(named, "pdf-backend").target(), server.address(),
                "the backend's target");
  require_equal(component(named, "pdf-backend").channels_size(), 0,
                "no channel exists before a parse needs the backend, and Health made none");
  require(!grparse::pdf_backend_channel_state(server.address()).has_value(),
          "Health did not create the backend channel");
  require_state(named, parsev1::SERVING_STATE_SERVING, "ready",
                "Health with a PDF backend named and not yet dialed");

  require(grparse::pdf_backend_channel(server.address())
              ->WaitForConnected(std::chrono::system_clock::now() + 5s),
          "the backend channel connects");
  const auto connected = server.health_rpc();
  require_component(connected, "pdf-backend", parsev1::COMPONENT_KIND_PDF_BACKEND,
                    parsev1::COMPONENT_STATE_CONNECTED);
  require_equal(channels_of(component(connected, "pdf-backend")), "CHANNEL_CONNECTIVITY_READY",
                "the backend's channel");

  setenv("GRPARSE_PDF_BACKEND", ("qparse:50070, " + server.address()).c_str(), 1);
  const auto consensus = server.health_rpc();
  int backends = 0;
  for (const auto& one : consensus.components()) {
    if (one.kind() != parsev1::COMPONENT_KIND_PDF_BACKEND) continue;
    require_equal(one.target(), backends == 0 ? "qparse:50070" : server.address(),
                  "a consensus list's targets, in order");
    ++backends;
  }
  require_equal(backends, 2, "one component per consensus target");
  setenv("GRPARSE_PDF_BACKEND", "pdfium:50069", 1);
}

// Health until `name` reaches `state`, or the last answer after ten seconds.
// A connection attempt settles on gRPC's threads, not this one.
parsev1::HealthResponse health_once(const HealthServer& server, std::string_view name,
                                    parsev1::ComponentState state) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  for (;;) {
    auto health = server.health_rpc();
    if (component(health, name).state() == state || std::chrono::steady_clock::now() > deadline) {
      return health;
    }
    std::this_thread::sleep_for(20ms);
  }
}

// Collectors and peers are reported from the channels the parses already
// hold: unset is NOT_CONFIGURED, named and never used is CONFIGURED with no
// channel, and after a parse has dialed, the state gRPC keeps for each
// channel of the pool. Health itself never creates or connects one.
void verify_health_reads_collector_channels_without_dialing() {
  setenv("GRPARSE_PDF_BACKEND", "pdfium:50069", 1);
  // A second server to dial, so the collector targets are not the server
  // under test.
  HealthServer peer(kEveryEngineLoaded);
  const std::string closed = closed_address();
  grparse::CollectorTargets targets;
  targets.email = peer.address();
  targets.xml = closed;
  targets.derender.target = peer.address();
  targets.channel_options.channels = 2;
  HealthServer server(kEveryEngineLoaded, targets);
  server.health->mark_startup_complete();

  const auto untouched = server.health_rpc();
  require_component(untouched, "email", parsev1::COMPONENT_KIND_COLLECTOR,
                    parsev1::COMPONENT_STATE_CONFIGURED);
  require_equal(parsev1::Collector_Name(component(untouched, "email").collector()),
                "COLLECTOR_EMAIL", "the collector a component stands for");
  require_equal(component(untouched, "email").target(), peer.address(), "the collector's target");
  require_component(untouched, "xml", parsev1::COMPONENT_KIND_COLLECTOR,
                    parsev1::COMPONENT_STATE_CONFIGURED);
  require_component(untouched, "markup", parsev1::COMPONENT_KIND_COLLECTOR,
                    parsev1::COMPONENT_STATE_NOT_CONFIGURED);
  require(!component(untouched, "markup").has_target(), "an unset collector has no target");
  require_component(untouched, "vlm-convert", parsev1::COMPONENT_KIND_COLLECTOR,
                    parsev1::COMPONENT_STATE_NOT_CONFIGURED);
  require_equal(parsev1::Collector_Name(component(untouched, "vlm-convert").collector()),
                "COLLECTOR_VLM", "the vlm convert leg's collector");
  require_component(untouched, "enrich", parsev1::COMPONENT_KIND_PEER,
                    parsev1::COMPONENT_STATE_CONFIGURED);
  require_component(untouched, "lighton-ocr", parsev1::COMPONENT_KIND_PEER,
                    parsev1::COMPONENT_STATE_NOT_CONFIGURED);
  for (const char* name : {"libreoffice", "calamine", "asr", "ebcdic", "epub", "lol-html",
                           "fastwarc", "pdf"}) {
    require_component(untouched, name, parsev1::COMPONENT_KIND_COLLECTOR,
                      parsev1::COMPONENT_STATE_NOT_CONFIGURED);
  }
  require_state(untouched, parsev1::SERVING_STATE_SERVING, "ready",
                "Health with collectors named and not yet dialed, and others unset");
  require(server.endpoints->channel_states(parsev1::COLLECTOR_EMAIL).empty() &&
              server.endpoints->channel_states(parsev1::COLLECTOR_XML).empty() &&
              server.endpoints->enrich_channel_states().empty(),
          "Health created no collector channel");

  // What a parse does: take the pool's next channel and use it.
  require(server.endpoints->channel(parsev1::COLLECTOR_EMAIL)
              ->WaitForConnected(std::chrono::system_clock::now() + 5s),
          "the email channel connects");
  require(server.endpoints->enrich_channel()->WaitForConnected(std::chrono::system_clock::now() +
                                                               5s),
          "the enrich channel connects");
  const auto dialed = server.health_rpc();
  require_component(dialed, "email", parsev1::COMPONENT_KIND_COLLECTOR,
                    parsev1::COMPONENT_STATE_CONNECTED);
  require_equal(channels_of(component(dialed, "email")),
                "CHANNEL_CONNECTIVITY_READY,CHANNEL_CONNECTIVITY_IDLE",
                "the email pool: the channel a call used, and the one no call has used yet");
  require_component(dialed, "enrich", parsev1::COMPONENT_KIND_PEER,
                    parsev1::COMPONENT_STATE_CONNECTED);
  require_state(dialed, parsev1::SERVING_STATE_SERVING, "ready",
                "Health with its dialed collectors connected");

  require(!server.endpoints->channel(parsev1::COLLECTOR_XML)
               ->WaitForConnected(std::chrono::system_clock::now() + 1s),
          "nothing listens at the xml target");
  const auto failing = health_once(server, "xml", parsev1::COMPONENT_STATE_UNREACHABLE);
  require_component(failing, "xml", parsev1::COMPONENT_KIND_COLLECTOR,
                    parsev1::COMPONENT_STATE_UNREACHABLE);
  require(channels_of(component(failing, "xml")).contains("CHANNEL_CONNECTIVITY_TRANSIENT_FAILURE"),
          "the xml pool shows the failed channel: " + channels_of(component(failing, "xml")));
  require_state(failing, parsev1::SERVING_STATE_SERVING_DEGRADED, "degraded",
                "Health with a configured collector that cannot be reached");
  server.publish();
  require_equal(health_check(server, parsev1::ParseService::service_full_name()), "SERVING",
                "grpc.health.v1 with an unreachable collector");
}

// A server whose health service was never enabled has nowhere to publish.
void verify_publishing_needs_the_health_service() {
  const grparse::ServiceHealth health(
      kEveryEngineLoaded, std::make_shared<grparse::CollectorEndpoints>(grparse::CollectorTargets{}));
  bool refused = false;
  try {
    grparse::publish_serving_status(health, nullptr);
  } catch (const std::logic_error&) {
    refused = true;
  }
  require(refused, "publishing to no health service is refused");
}

}  // namespace

int main() {
  return grparse_test::run_test_main(
      "service-health-test", "ok",
      {verify_health_is_not_serving_until_startup_completes,
       verify_a_service_without_a_health_state_is_not_serving,
       verify_health_names_the_engines_startup_did_not_build,
       verify_health_reports_the_pdf_backend,
       verify_health_reads_collector_channels_without_dialing,
       verify_publishing_needs_the_health_service});
}
