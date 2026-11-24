#include "grparse/service_health.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ai/pipestream/parse/v1/parse.grpc.pb.h"
#include "ai/pipestream/parse/v1/parse_stream.grpc.pb.h"
#include "grparse/collector_coordinator.h"
#include "grparse/consensus_page_source.h"
#include "grparse/remote_page_source.h"

namespace parsev1 = ai::pipestream::parse::v1;

namespace grparse {

namespace {

// The collectors CollectorEndpoints::target() knows a dial target for, in
// Collector order. One that gains a GRPARSE_<NAME>_TARGET joins both.
constexpr parsev1::Collector kDialedCollectors[] = {
    parsev1::COLLECTOR_LIBREOFFICE, parsev1::COLLECTOR_CALAMINE, parsev1::COLLECTOR_ASR,
    parsev1::COLLECTOR_EMAIL,       parsev1::COLLECTOR_XML,      parsev1::COLLECTOR_EBCDIC,
    parsev1::COLLECTOR_EPUB,        parsev1::COLLECTOR_MARKUP,   parsev1::COLLECTOR_LOL_HTML,
    parsev1::COLLECTOR_FASTWARC,    parsev1::COLLECTOR_PDF,
};

void add_engine(parsev1::HealthResponse* response, const char* name, EngineState state) {
  auto* engine = response->add_components();
  engine->set_name(name);
  engine->set_kind(parsev1::COMPONENT_KIND_ENGINE);
  switch (state) {
    case EngineState::kLoaded: engine->set_state(parsev1::COMPONENT_STATE_LOADED); break;
    case EngineState::kDisabled: engine->set_state(parsev1::COMPONENT_STATE_DISABLED); break;
    case EngineState::kNotLoaded: engine->set_state(parsev1::COMPONENT_STATE_NOT_LOADED); break;
  }
}

parsev1::ChannelConnectivity connectivity(grpc_connectivity_state state) {
  switch (state) {
    case GRPC_CHANNEL_IDLE: return parsev1::CHANNEL_CONNECTIVITY_IDLE;
    case GRPC_CHANNEL_CONNECTING: return parsev1::CHANNEL_CONNECTIVITY_CONNECTING;
    case GRPC_CHANNEL_READY: return parsev1::CHANNEL_CONNECTIVITY_READY;
    case GRPC_CHANNEL_TRANSIENT_FAILURE: return parsev1::CHANNEL_CONNECTIVITY_TRANSIENT_FAILURE;
    case GRPC_CHANNEL_SHUTDOWN: return parsev1::CHANNEL_CONNECTIVITY_SHUTDOWN;
  }
  return parsev1::CHANNEL_CONNECTIVITY_UNSPECIFIED;
}

// One dialed target. An empty `target` is an unset variable. A named one is
// judged by the channels that exist: connected when any is READY (a call can
// go out now), unreachable when none is and one is in TRANSIENT_FAILURE (a
// channel stays there through its backoff until a connection succeeds, so
// this does not flicker between attempts), and otherwise only configured,
// since an idle or absent channel says nothing about the peer.
parsev1::ComponentHealth* add_target(parsev1::HealthResponse* response, const char* name,
                                     parsev1::ComponentKind kind, const std::string& target,
                                     const std::vector<grpc_connectivity_state>& channels) {
  auto* component = response->add_components();
  component->set_name(name);
  component->set_kind(kind);
  if (target.empty()) {
    component->set_state(parsev1::COMPONENT_STATE_NOT_CONFIGURED);
    return component;
  }
  component->set_target(target);
  bool ready = false;
  bool failing = false;
  for (const grpc_connectivity_state state : channels) {
    component->add_channels(connectivity(state));
    ready = ready || state == GRPC_CHANNEL_READY;
    failing = failing || state == GRPC_CHANNEL_TRANSIENT_FAILURE;
  }
  component->set_state(ready     ? parsev1::COMPONENT_STATE_CONNECTED
                       : failing ? parsev1::COMPONENT_STATE_UNREACHABLE
                                 : parsev1::COMPONENT_STATE_CONFIGURED);
  return component;
}

void add_pdf_backends(parsev1::HealthResponse* response) {
  std::optional<std::string> configured;
  try {
    configured = remote_pdf_backend_target();
  } catch (const std::invalid_argument&) {
    // A value the PDF page source refuses (whitespace only, "inprocess")
    // names no backend a PDF could be read through. The server never gets
    // here: startup reads the same value and stops on it.
  }
  const std::vector<std::string> targets =
      configured.has_value() ? split_backend_targets(*configured) : std::vector<std::string>{};
  if (targets.empty()) {
    add_target(response, "pdf-backend", parsev1::COMPONENT_KIND_PDF_BACKEND, {}, {});
    return;
  }
  for (const std::string& target : targets) {
    std::vector<grpc_connectivity_state> channels;
    if (const auto state = pdf_backend_channel_state(target)) channels.push_back(*state);
    add_target(response, "pdf-backend", parsev1::COMPONENT_KIND_PDF_BACKEND, target, channels);
  }
}

// Whether this component's state takes something from requests that the
// deployment was set up to deliver (SERVING_STATE_SERVING_DEGRADED in
// parse.proto). A switched-off engine and an unset collector are choices; an
// unset PDF backend is not treated as one, because PDFs are what the
// in-process path exists for and each one then fails.
bool degrades(const parsev1::ComponentHealth& component) {
  switch (component.state()) {
    case parsev1::COMPONENT_STATE_NOT_LOADED:
    case parsev1::COMPONENT_STATE_UNREACHABLE:
      return true;
    case parsev1::COMPONENT_STATE_NOT_CONFIGURED:
      return component.kind() == parsev1::COMPONENT_KIND_PDF_BACKEND;
    default:
      return false;
  }
}

}  // namespace

ServiceHealth::ServiceHealth(EngineInventory engines,
                             std::shared_ptr<const CollectorEndpoints> endpoints)
    : engines_(engines), endpoints_(std::move(endpoints)) {}

void ServiceHealth::describe(parsev1::HealthResponse* response) const {
  response->clear_components();
  add_engine(response, "ocr", engines_.ocr);
  add_engine(response, "layout", engines_.layout);
  add_engine(response, "table-structure", engines_.table_structure);
  add_engine(response, "figure-classifier", engines_.figure_classifier);
  add_engine(response, "embeddings", engines_.embeddings);
  add_pdf_backends(response);
  for (const parsev1::Collector id : kDialedCollectors) {
    add_target(response, collector_name(id), parsev1::COMPONENT_KIND_COLLECTOR,
               endpoints_->target(id), endpoints_->channel_states(id))
        ->set_collector(id);
  }
  add_target(response, collector_name(parsev1::COLLECTOR_VLM), parsev1::COMPONENT_KIND_COLLECTOR,
             endpoints_->vlm().target, endpoints_->channel_states(parsev1::COLLECTOR_VLM))
      ->set_collector(parsev1::COLLECTOR_VLM);
  add_target(response, "enrich", parsev1::COMPONENT_KIND_PEER, endpoints_->derender().target,
             endpoints_->enrich_channel_states());
  add_target(response, "lighton-ocr", parsev1::COMPONENT_KIND_PEER, endpoints_->lighton().target,
             endpoints_->lighton_channel_states());

  const bool degraded = std::ranges::any_of(response->components(), degrades);
  const parsev1::ServingState state =
      !startup_complete_.load() ? parsev1::SERVING_STATE_NOT_SERVING
      : degraded                ? parsev1::SERVING_STATE_SERVING_DEGRADED
                                : parsev1::SERVING_STATE_SERVING;
  response->set_state(state);
  response->set_status(serving_status_text(state));
}

parsev1::ServingState ServiceHealth::state() const {
  parsev1::HealthResponse response;
  describe(&response);
  return response.state();
}

const char* serving_status_text(parsev1::ServingState state) {
  switch (state) {
    case parsev1::SERVING_STATE_SERVING: return "ready";
    case parsev1::SERVING_STATE_SERVING_DEGRADED: return "degraded";
    default: return "starting";
  }
}

void publish_serving_status(const ServiceHealth& health,
                            grpc::HealthCheckServiceInterface* service) {
  if (service == nullptr) {
    throw std::logic_error(
        "the gRPC health service is not enabled, so the serving status cannot be published");
  }
  // Degraded is still serving here: grpc.health.v1 has one bit, and taking
  // an instance out of rotation over a missing optional part is a decision
  // the typed Health answer leaves to whoever reads it.
  const bool serving = health.state() != parsev1::SERVING_STATE_NOT_SERVING;
  service->SetServingStatus("", serving);
  service->SetServingStatus(parsev1::ParseService::service_full_name(), serving);
  service->SetServingStatus(parsev1::ParseStreamingService::service_full_name(), serving);
}

}  // namespace grparse
