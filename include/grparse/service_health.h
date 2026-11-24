#pragma once

#include <atomic>
#include <memory>

#include <grpcpp/health_check_service_interface.h>

#include "ai/pipestream/parse/v1/parse.pb.h"
#include "grparse/document_parser_service.h"

namespace grparse {

// What the Health RPC and grpc.health.v1 answer, computed in one place from
// what the process actually holds. Both used to answer "ready" and SERVING
// from the moment the port was bound, whatever had been built or configured
// (#412), so an instance with no PDF backend, or one whose layout model was
// never built, looked the same to an orchestrator as a complete one.
//
// Nothing here probes. An engine's state is the one startup recorded, and a
// target's is the connectivity state gRPC already keeps for the channels the
// parses use, so Health stays as cheap as the constant it replaces.

// How startup left one in-process engine.
enum class EngineState {
  // Its setting asked for it and it was not built. Startup stops rather
  // than leave layout, table structure or the figure classifier so, which
  // makes this the state of an inventory nobody filled in.
  kNotLoaded,
  // Its setting switched it off.
  kDisabled,
  // Built and in use.
  kLoaded,
};

// The in-process engines, as the code that built them reports them. Every
// one starts kNotLoaded: an inventory nobody filled in has loaded nothing.
struct EngineInventory {
  EngineState ocr = EngineState::kNotLoaded;
  EngineState layout = EngineState::kNotLoaded;
  EngineState table_structure = EngineState::kNotLoaded;
  EngineState figure_classifier = EngineState::kNotLoaded;
  EngineState embeddings = EngineState::kNotLoaded;
};

class ServiceHealth {
 public:
  // `engines` is what startup built; `endpoints` are the collectors and
  // peers the parses dial, whose existing channels are read on every call.
  ServiceHealth(EngineInventory engines, std::shared_ptr<const CollectorEndpoints> endpoints);

  // Called once everything the settings ask for has been built and the
  // server is ready to take requests. Until then the state is
  // SERVING_STATE_NOT_SERVING whatever the components say.
  void mark_startup_complete() { startup_complete_.store(true); }

  // Fills `status`, `state` and `components` (not `version`) with the state
  // at the time of the call. GRPARSE_PDF_BACKEND is read here, per call, as
  // the PDF page source reads it per document.
  void describe(ai::pipestream::parse::v1::HealthResponse* response) const;

  // The `state` describe() reports.
  ai::pipestream::parse::v1::ServingState state() const;

 private:
  EngineInventory engines_;
  std::shared_ptr<const CollectorEndpoints> endpoints_;
  std::atomic<bool> startup_complete_{false};
};

// HealthResponse.status for `state`: "starting", "ready" or "degraded".
const char* serving_status_text(ai::pipestream::parse::v1::ServingState state);

// Sets grpc.health.v1 from `health`: the server as a whole ("") and both
// parsing services by name answer NOT_SERVING while `health` is
// SERVING_STATE_NOT_SERVING and SERVING otherwise. gRPC's default health
// service answers SERVING for "" from the moment the server starts and
// knows no service by name, so the server calls this right after it binds
// and again when startup completes. `service` is the server's
// GetHealthCheckService(); null (the default service was not enabled)
// throws std::logic_error, since a server that cannot report its health
// must not start as if it could.
void publish_serving_status(const ServiceHealth& health,
                            grpc::HealthCheckServiceInterface* service);

}  // namespace grparse
