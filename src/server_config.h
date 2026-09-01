#pragma once

// Every environment variable the server reads, grouped by the concern that
// owns it. Each group is read where the startup sequence needs it, so a
// value the server refuses still stops the process at the point that value
// would have mattered, and the process prints what it had already settled.
//
// Nothing here is silently defaulted past a bad value: a variable that is
// set but unusable throws std::invalid_argument naming the variable and the
// range it accepts, which main reports as "Startup failed: ...".

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "grparse/document_parser_service.h"
#include "grparse/document_repair.h"
#include "grparse/figure_classifier.h"
#include "grparse/layout_engine.h"
#include "grparse/ocr_engine.h"
#include "grparse/page_scheduler.h"
#include "grparse/table_structure_engine.h"

namespace grparse {

// GRPARSE_MODELS_DIR and GRPARSE_LISTEN_ADDRESS: the two values the process
// needs before it builds anything. Neither can fail; an unset variable takes
// its default.
struct ProcessConfig {
  std::string listen_address;
  std::filesystem::path models_dir;
};

ProcessConfig read_process_config();

// The sizing every model pool and the page scheduler are built from.
// GRPARSE_PAGE_WORKERS, GRPARSE_RENDER_WORKERS, GRPARSE_CUDA_DEVICE and
// GRPARSE_INTRA_OP_THREADS, plus the machine's own core count.
struct WorkerConfig {
  size_t inference_workers = 0;
  size_t render_workers = 0;
  int gpu_index = 0;
  size_t cores = 1;
  int intra_op_threads = 1;
};

WorkerConfig read_worker_config();

// The gRPC server's own resource bounds: GRPARSE_GRPC_MEMORY_MIB,
// GRPARSE_GRPC_MAX_THREADS and GRPARSE_MAX_CONCURRENT_STREAMS.
struct GrpcLimits {
  size_t memory_mib = 0;
  size_t max_threads = 0;
  size_t max_concurrent_streams = 0;
};

GrpcLimits read_grpc_limits();

// GRPARSE_METRICS_PORT (0 keeps the Prometheus listener off) and
// GRPARSE_METRICS_INTERVAL_SECONDS (0 keeps the stdout line off).
struct MetricsConfig {
  int port = 0;
  int interval_seconds = 0;
};

MetricsConfig read_metrics_config();

// The queue capacities, worker counts and page options the scheduler runs
// with, plus the recognition switches whose answer depends on which engines
// actually came up. Prints the lines that say which optional features are
// on, in the order the startup log has always carried them.
PageScheduler::Options read_scheduler_options(const WorkerConfig& workers, bool layout_active,
                                              bool classifier_active);

// GRPARSE_<COLLECTOR>_TARGET names each remote collector's endpoint.
// Unset leaves that collector unconfigured: documents routed to it then
// fail that collector with a clear error instead of being converted
// through any intermediate. GRPARSE_ASR_MODEL names the whisper model
// grpc-asr must serve; the asr wire requires one.
CollectorTargets read_collector_targets();

// One startup line per collector, then the chart derender leg and the
// office CV enrichment leg, whose answers depend on which engines came up.
void report_collector_targets(const CollectorTargets& targets, bool layout_active,
                              bool classifier_active);

// The unary executor's pool: GRPARSE_UNARY_WORKERS and GRPARSE_UNARY_QUEUE.
CallExecutor::Options read_executor_options();

// GRPARSE_REPAIR: on (default) runs the post-merge repair pass on every
// finished Document (running headers and footers demoted to furniture,
// line-break hyphenation rejoined, paragraphs a page break split merged);
// off skips it; debug runs it and prints one line per document it changed.
std::optional<RepairOptions> configure_repair();

// GRPARSE_ORT_EP selects the ONNX Runtime execution provider.  cuda is
// the default and keeps the fail-loud behaviour; cpu is a deliberate choice,
// never a silent fallback; openvino targets Intel GPUs/NPUs through the
// OpenVINO build of ONNX Runtime; auto prefers CUDA, then OpenVINO, then CPU,
// logging each fallback.  Requesting a provider this binary was not built
// with fails with the list that is actually available.
std::unique_ptr<OcrEnginePool> build_engine_pool(const std::filesystem::path& models,
                                                 size_t worker_count, int gpu_index);

// GRPARSE_LAYOUT: auto (default) enables layout labelling when the model file
// exists; on requires it and fails startup when absent; off disables it.
// GRPARSE_LAYOUT_MODEL picks which detector that file is.  Nothing here
// degrades silently: auto logs which way it went, and an explicitly selected
// model that is not on disk stops the process.
std::unique_ptr<LayoutEngine> build_layout_engine(const std::filesystem::path& models_dir);

// GRPARSE_TABLE_STRUCTURE follows the same auto/on/off contract as layout.
// Structure only ever sees crops of layout-detected table regions, so it
// additionally requires layout to be active.
std::unique_ptr<TableStructureEnginePool> build_table_structure_pool(
    const std::filesystem::path& models_dir, size_t worker_count, bool layout_active);

// GRPARSE_FIGURE_CLASSES follows the same auto/on/off contract; the
// classifier only ever sees crops of layout-detected figure regions.
std::unique_ptr<FigureClassifierPool> build_figure_classifier_pool(
    const std::filesystem::path& models_dir, size_t worker_count, bool layout_active);

}  // namespace grparse
