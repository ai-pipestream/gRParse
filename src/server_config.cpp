#include "server_config.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <print>
#include <stdexcept>
#include <thread>

#include "grparse/chart_derender.h"
#include "grparse_session_ep.h"

namespace grparse {
namespace {

// A count with a floor of 1: unset takes `fallback`, anything set must parse
// as an integer inside [1, maximum] or the process refuses to start.
size_t configured_size(const char* name, size_t fallback, size_t maximum = 1024) {
  const char* configured = std::getenv(name);
  if (configured != nullptr) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(configured, &end, 10);
    if (end != configured && *end == '\0' && parsed > 0 && parsed <= maximum) {
      return static_cast<size_t>(parsed);
    }
    throw std::invalid_argument(std::string(name) + " must be an integer between 1 and " +
                                std::to_string(maximum));
  }
  return fallback;
}

// An index or a switch whose 0 is meaningful: same contract, floor of 0.
int configured_index(const char* name, int fallback, int maximum = 63) {
  const char* configured = std::getenv(name);
  if (configured == nullptr) return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(configured, &end, 10);
  if (end == configured || *end != '\0' || parsed < 0 || parsed > maximum) {
    throw std::invalid_argument(std::string(name) + " must be an integer between 0 and " +
                                std::to_string(maximum));
  }
  return static_cast<int>(parsed);
}

// The accepted words of a mode variable, written the way the rejection has
// always read them out: "on or off" for a pair, "auto, on, or off" for more.
std::string spell_out(std::initializer_list<const char*> allowed) {
  std::string accepted;
  size_t remaining = allowed.size();
  for (const char* word : allowed) {
    --remaining;
    if (!accepted.empty()) accepted += allowed.size() > 2 ? ", " : " ";
    if (remaining == 0) accepted += "or ";
    accepted += word;
  }
  return accepted;
}

// A variable whose value is one of a small set of words; unset (or empty)
// takes `fallback`. The rejection names the variable and every word it
// accepts, which is the only thing a caller can act on.
std::string configured_mode(const char* name, const char* fallback,
                            std::initializer_list<const char*> allowed) {
  const char* configured = std::getenv(name);
  const std::string mode =
      configured == nullptr || *configured == '\0' ? std::string(fallback) : configured;
  if (std::ranges::find(allowed, mode) != allowed.end()) return mode;
  throw std::invalid_argument(std::string(name) + " must be " + spell_out(allowed));
}

std::string collector_env(const char* name) {
  const char* configured = std::getenv(name);
  return configured == nullptr ? std::string() : std::string(configured);
}

bool provider_available(const char* name) {
  const auto providers = Ort::GetAvailableProviders();
  return std::ranges::find(providers, std::string(name)) != providers.end();
}

std::string available_providers() {
  std::string joined;
  for (const auto& provider : Ort::GetAvailableProviders()) {
    if (!joined.empty()) joined += ", ";
    joined += provider;
  }
  return joined;
}

std::string configured_openvino_device() {
  const char* configured = std::getenv("GRPARSE_OPENVINO_DEVICE");
  const std::string device =
      configured == nullptr || *configured == '\0' ? "GPU" : configured;
  // GPU, GPU.1, CPU, NPU, AUTO:GPU,CPU, HETERO: and friends: validate the
  // alphabet and let OpenVINO reject unknown devices itself, at startup.
  for (const char letter : device) {
    if (std::isalnum(static_cast<unsigned char>(letter)) == 0 && letter != '.' &&
        letter != ':' && letter != ',' && letter != '_' && letter != '-') {
      throw std::invalid_argument("GRPARSE_OPENVINO_DEVICE contains unsupported characters");
    }
  }
  return device;
}

// GRPARSE_OPENVINO_CACHE_DIR: where the OpenVINO plugin may keep compiled
// blobs, so a restart reuses the previous compile instead of repeating it.
// Unset means no cache, which is the safe default for a read-only container.
std::string configured_openvino_cache_dir() {
  const char* configured = std::getenv("GRPARSE_OPENVINO_CACHE_DIR");
  return configured == nullptr ? std::string() : std::string(configured);
}

// Builds the warm session pool with an explicit provider selection and proves
// the RapidOcr hook actually ran: a dependency tree built without
// patches/rapidocr-session-ep.patch would otherwise run CPU silently.
std::unique_ptr<grparse::OcrEnginePool> build_pool_with(grparse::OrtEp ep,
                                                        const std::filesystem::path& models,
                                                        size_t worker_count, int gpu_index) {
  grparse::OrtEpSelection selection{
      .ep = ep,
      .cuda_device = gpu_index,
      .openvino_device = configured_openvino_device(),
      .openvino_cache_dir = configured_openvino_cache_dir(),
  };
  grparse::set_ort_ep_selection(selection);
  auto pool = std::make_unique<grparse::OcrEnginePool>(models, worker_count, -1);
  if (grparse::ep_hook_invocations() == 0) {
    throw std::runtime_error(
        "RapidOcr session hook never ran: the rapidocr dependency was built without "
        "patches/rapidocr-session-ep.patch; rebuild with a fresh dependency cache");
  }
  return pool;
}

size_t page_worker_count() {
  const unsigned int hardware = std::thread::hardware_concurrency();
  return std::min<size_t>(2, hardware == 0 ? 1 : hardware);
}

// GRPARSE_BARCODES: auto (default) decodes figure crops whose top classifier
// call is bar_code or qr_code, so it needs the classifier; on decodes every
// figure crop (needs only layout); off disables decoding.  ZXing is compiled
// in, so no model file gates this.
grparse::PageScheduler::BarcodeMode configure_barcode_mode(bool layout_active,
                                                           bool classifier_active) {
  using BarcodeMode = grparse::PageScheduler::BarcodeMode;
  const std::string mode = configured_mode("GRPARSE_BARCODES", "auto", {"auto", "on", "off"});
  if (mode == "off") {
    std::println("gRParse barcodes: disabled (GRPARSE_BARCODES=off)");
    return BarcodeMode::kOff;
  }
  if (mode == "on") {
    if (!layout_active) {
      throw std::invalid_argument("GRPARSE_BARCODES=on needs layout enabled to find figure regions");
    }
    std::println("gRParse barcodes: enabled for all figure crops (GRPARSE_BARCODES=on)");
    return BarcodeMode::kAll;
  }
  if (!classifier_active) {
    std::println("gRParse barcodes: disabled (figure classes are disabled; "
                 "GRPARSE_BARCODES=on decodes without the classifier)");
    return BarcodeMode::kOff;
  }
  std::println("gRParse barcodes: enabled for bar_code/qr_code figure classes");
  return BarcodeMode::kClassTriggered;
}

// GRPARSE_PICTURE_IMAGES=on embeds PNG crops of figure regions in picture
// items.  Off by default: crops inflate page events on figure-heavy docs.
bool configure_picture_images(bool layout_active) {
  const std::string mode = configured_mode("GRPARSE_PICTURE_IMAGES", "off", {"on", "off"});
  const bool capture = mode == "on" && layout_active;
  if (mode == "on" && !layout_active) {
    std::println("gRParse picture images: disabled (layout is disabled)");
  } else if (capture) {
    std::println("gRParse picture images: enabled");
  }
  return capture;
}

// GRPARSE_PAGE_IMAGES=on embeds a downscaled PNG preview of every page
// raster in the page event, so clients can paint boxes over the real
// page.  Off by default: previews add bytes to every page event.  Unlike
// picture images this needs no layout model; it forces rasterization of
// full-digital pages instead.
bool configure_page_images() {
  const bool capture = configured_mode("GRPARSE_PAGE_IMAGES", "off", {"on", "off"}) == "on";
  if (capture) std::println("gRParse page images: enabled");
  return capture;
}

// GRPARSE_OCR_ROTATION=on (default) re-reads a layerless page whose
// first read says the raster was turned (tall line boxes, an upside-down
// classifier vote, or a poor read) at the turns the evidence names and
// keeps the best read; off never re-reads.
bool configure_ocr_rotation() {
  const bool enabled = configured_mode("GRPARSE_OCR_ROTATION", "on", {"on", "off"}) == "on";
  std::println("gRParse OCR orientation recovery: {}",
               enabled ? "enabled" : "disabled (GRPARSE_OCR_ROTATION=off)");
  return enabled;
}

}  // namespace

ProcessConfig read_process_config() {
  const char* models = std::getenv("GRPARSE_MODELS_DIR");
  const char* address = std::getenv("GRPARSE_LISTEN_ADDRESS");
  return ProcessConfig{
      .listen_address = address == nullptr ? "0.0.0.0:50051" : address,
      .models_dir = models == nullptr ? "/models" : models,
  };
}

WorkerConfig read_worker_config() {
  WorkerConfig workers;
  workers.inference_workers = configured_size("GRPARSE_PAGE_WORKERS", page_worker_count(), 64);
  const unsigned int hardware = std::thread::hardware_concurrency();
  workers.render_workers = configured_size(
      "GRPARSE_RENDER_WORKERS", std::min<size_t>(4, hardware == 0 ? 2 : hardware), 256);
  workers.gpu_index = configured_index("GRPARSE_CUDA_DEVICE", 0);
  // Pooled sessions split the machine instead of each claiming all of it.
  // ONNX Runtime's default is every core per session, so a pool of them is
  // oversubscribed by exactly the worker count - which on small machines
  // costs more than the extra worker earns.  The single shared layout
  // session is exempt and asks for all cores itself.
  workers.cores = hardware == 0 ? 1 : hardware;
  workers.intra_op_threads = configured_index(
      "GRPARSE_INTRA_OP_THREADS",
      static_cast<int>(std::max<size_t>(1, workers.cores / workers.inference_workers)), 1024);
  return workers;
}

GrpcLimits read_grpc_limits() {
  return GrpcLimits{
      .memory_mib = configured_size("GRPARSE_GRPC_MEMORY_MIB", 640, 16384),
      .max_threads = configured_size("GRPARSE_GRPC_MAX_THREADS", 64, 1024),
      // Per connection, not per server: one client channel may have 32 RPCs in
      // flight while the unary executor admits many more across all clients.
      // Deliberate: the cap is a per-peer fairness bound, so a single client
      // cannot fill the conversion queue on its own, and a client that wants
      // more concurrency opens more channels.
      .max_concurrent_streams = configured_size("GRPARSE_MAX_CONCURRENT_STREAMS", 32, 1024),
  };
}

MetricsConfig read_metrics_config() {
  MetricsConfig metrics;
  // GRPARSE_METRICS_PORT exposes the scheduler counters in Prometheus text
  // format at /metrics.  0 (the default) keeps the listener off; a
  // configured port that cannot be bound fails startup loudly.
  metrics.port = configured_index("GRPARSE_METRICS_PORT", 0, 65535);
  // Pipeline visibility: one metrics line per interval on stdout, where
  // container logging already looks.  0 disables.
  metrics.interval_seconds = configured_index("GRPARSE_METRICS_INTERVAL_SECONDS", 60, 86400);
  return metrics;
}

PageScheduler::Options read_scheduler_options(const WorkerConfig& workers, bool layout_active,
                                              bool classifier_active) {
  // Named assignment on purpose: a positional brace list of nine same-typed
  // sizes is one reordering away from a silent misconfiguration.
  PageScheduler::Options options;
  options.document_queue_capacity = configured_size("GRPARSE_DOCUMENT_QUEUE", 8);
  options.render_queue_capacity = configured_size("GRPARSE_RENDER_QUEUE", 8);
  options.inference_queue_capacity = configured_size("GRPARSE_INFERENCE_QUEUE", 4);
  options.assembly_queue_capacity = configured_size("GRPARSE_ASSEMBLY_QUEUE", 8);
  options.render_workers = workers.render_workers;
  options.inference_workers = workers.inference_workers;
  options.assembly_workers = configured_size("GRPARSE_ASSEMBLY_WORKERS", 2, 64);
  options.page_window = configured_size("GRPARSE_PAGE_WINDOW", 4, 64);
  options.max_active_documents = configured_size("GRPARSE_MAX_ACTIVE_DOCUMENTS", 32, 1024);
  options.pdf_parsers = configured_size("GRPARSE_PDF_PARSERS", workers.render_workers, 256);
  options.capture_picture_images = configure_picture_images(layout_active);
  options.capture_page_images = configure_page_images();
  options.barcode_mode = configure_barcode_mode(layout_active, classifier_active);
  options.orientation.enabled = configure_ocr_rotation();
  return options;
}

CollectorTargets read_collector_targets() {
  return CollectorTargets{
      .libreoffice = collector_env("GRPARSE_LIBREOFFICE_TARGET"),
      .asr = collector_env("GRPARSE_ASR_TARGET"),
      .asr_model = collector_env("GRPARSE_ASR_MODEL"),
      .email = collector_env("GRPARSE_EMAIL_TARGET"),
      .xml = collector_env("GRPARSE_XML_TARGET"),
      .ebcdic = collector_env("GRPARSE_EBCDIC_TARGET"),
      .epub = collector_env("GRPARSE_EPUB_TARGET"),
      .markup = collector_env("GRPARSE_MARKUP_TARGET"),
      .lol_html = collector_env("GRPARSE_LOL_HTML_TARGET"),
      .fastwarc = collector_env("GRPARSE_FASTWARC_TARGET"),
      .pdf = collector_env("GRPARSE_PDF_TARGET"),
      // The chart derender leg through grpc-enrich: off unless a target
      // is named; the timeout bounds the whole leg per parse.
      .derender = ChartDerenderOptions{
          .target = collector_env("GRPARSE_ENRICH_TARGET"),
          .timeout = std::chrono::milliseconds(
              configured_size("GRPARSE_ENRICH_TIMEOUT_MS", 5000, 600000)),
          .vlm_endpoint = collector_env("GRPARSE_ENRICH_VLM_ENDPOINT"),
      },
  };
}

void report_collector_targets(const CollectorTargets& targets, bool layout_active,
                              bool classifier_active) {
  const auto report_collector = [](const char* name, const std::string& target) {
    std::println("gRParse {} collector: {}", name,
                 target.empty() ? "not configured" : target);
  };
  report_collector("libreoffice", targets.libreoffice);
  report_collector("asr", targets.asr);
  if (!targets.asr.empty()) {
    std::println("gRParse asr model: {}",
                 targets.asr_model.empty() ? "NOT CONFIGURED (GRPARSE_ASR_MODEL)"
                                           : targets.asr_model);
  }
  report_collector("email", targets.email);
  report_collector("xml", targets.xml);
  report_collector("ebcdic", targets.ebcdic);
  report_collector("epub", targets.epub);
  report_collector("markup", targets.markup);
  report_collector("lol-html", targets.lol_html);
  report_collector("fastwarc", targets.fastwarc);
  report_collector("pdf", targets.pdf);
  if (targets.derender.enabled()) {
    std::println("gRParse chart derender (enrich): {} ({} ms{})", targets.derender.target,
                 targets.derender.timeout.count(),
                 targets.derender.vlm_endpoint.empty()
                     ? std::string()
                     : ", vlm " + targets.derender.vlm_endpoint);
  } else {
    std::println("gRParse chart derender (enrich): not configured");
  }
  if (!targets.libreoffice.empty()) {
    std::println("gRParse office CV enrichment: {}",
                 layout_active ? "enabled (layout"
                                     + std::string(classifier_active ? " + figure classes" : "")
                                     + ")"
                               : "disabled (layout is disabled)");
  }
}

CallExecutor::Options read_executor_options() {
  // The unary surfaces run on gRPC's callback API, so their parsing blocks
  // on this pool instead of on an event-manager thread. A worker spends
  // nearly all its life waiting on the scheduler or on a collector, so the
  // count bounds concurrent conversions rather than CPU use; past the queue
  // a conversion is refused with RESOURCE_EXHAUSTED instead of queued behind
  // its own deadline.
  CallExecutor::Options executor_options;
  executor_options.workers = configured_size("GRPARSE_UNARY_WORKERS", 16, 512);
  executor_options.queue_capacity = configured_size("GRPARSE_UNARY_QUEUE", 64, 4096);
  return executor_options;
}

std::optional<RepairOptions> configure_repair() {
  const char* configured = std::getenv("GRPARSE_REPAIR");
  const std::string mode = configured == nullptr ? "on" : configured;
  if (mode != "on" && mode != "off" && mode != "debug") {
    throw std::invalid_argument("GRPARSE_REPAIR must be on, off, or debug");
  }
  if (mode == "off") {
    std::println("gRParse document repair: disabled (GRPARSE_REPAIR=off)");
    return std::nullopt;
  }
  RepairOptions options;
  options.log_report = mode == "debug";
  std::println("gRParse document repair: enabled{}",
               options.log_report ? " with per-document log lines (GRPARSE_REPAIR=debug)" : "");
  return options;
}

std::unique_ptr<OcrEnginePool> build_engine_pool(const std::filesystem::path& models,
                                                 size_t worker_count, int gpu_index) {
  const char* configured = std::getenv("GRPARSE_ORT_EP");
  const std::string ep = configured == nullptr || *configured == '\0' ? "cuda" : configured;
  if (ep == "cuda") {
    if (!provider_available("CUDAExecutionProvider")) {
      throw std::invalid_argument(
          "GRPARSE_ORT_EP=cuda: this build's ONNX Runtime has no CUDA execution provider "
          "(available: " + available_providers() + ")");
    }
    auto pool = build_pool_with(grparse::OrtEp::kCuda, models, worker_count, gpu_index);
    std::println("gRParse OCR execution provider: CUDA (device {})", gpu_index);
    return pool;
  }
  if (ep == "openvino") {
    if (!provider_available("OpenVINOExecutionProvider")) {
      throw std::invalid_argument(
          "GRPARSE_ORT_EP=openvino: this build's ONNX Runtime has no OpenVINO execution "
          "provider (available: " + available_providers() + "); use the image built from "
          "Dockerfile.openvino");
    }
    auto pool = build_pool_with(grparse::OrtEp::kOpenVino, models, worker_count, gpu_index);
    std::println("gRParse OCR execution provider: OpenVINO ({})", configured_openvino_device());
    return pool;
  }
  if (ep == "cpu") {
    auto pool = build_pool_with(grparse::OrtEp::kCpu, models, worker_count, gpu_index);
    std::println("gRParse OCR execution provider: CPU (GRPARSE_ORT_EP=cpu)");
    return pool;
  }
  if (ep == "auto") {
    if (provider_available("CUDAExecutionProvider")) {
      try {
        auto pool = build_pool_with(grparse::OrtEp::kCuda, models, worker_count, gpu_index);
        std::println("gRParse OCR execution provider: CUDA (device {}, selected by GRPARSE_ORT_EP=auto)",
                     gpu_index);
        return pool;
      } catch (const std::exception& error) {
        std::println(stderr, "GRPARSE_ORT_EP=auto: CUDA initialization failed ({})", error.what());
      }
    }
    if (provider_available("OpenVINOExecutionProvider")) {
      try {
        auto pool = build_pool_with(grparse::OrtEp::kOpenVino, models, worker_count, gpu_index);
        std::println("gRParse OCR execution provider: OpenVINO ({}, selected by GRPARSE_ORT_EP=auto)",
                     configured_openvino_device());
        return pool;
      } catch (const std::exception& error) {
        std::println(stderr, "GRPARSE_ORT_EP=auto: OpenVINO initialization failed ({})",
                     error.what());
      }
    }
    auto pool = build_pool_with(grparse::OrtEp::kCpu, models, worker_count, gpu_index);
    std::println("gRParse OCR execution provider: CPU (selected by GRPARSE_ORT_EP=auto)");
    return pool;
  }
  throw std::invalid_argument("GRPARSE_ORT_EP must be cuda, openvino, cpu, or auto");
}

std::unique_ptr<LayoutEngine> build_layout_engine(const std::filesystem::path& models_dir) {
  const std::string mode = configured_mode("GRPARSE_LAYOUT", "auto", {"auto", "on", "off"});
  const grparse::LayoutModel selection = grparse::configured_layout_model();
  const std::filesystem::path model = models_dir / grparse::layout_model_file(selection);
  if (mode == "off") {
    std::println("gRParse layout: disabled (GRPARSE_LAYOUT=off)");
    return nullptr;
  }
  if (mode == "auto" && !std::filesystem::exists(model)) {
    std::println("gRParse layout: disabled (no {}; see models/README.md)", model.string());
    return nullptr;
  }
  // "on" with a missing file reaches the engine constructor, which throws with
  // the model path and the selection - the fail-loud startup that setting asks
  // for.  One session serves every inference worker.
  auto engine = std::make_unique<grparse::LayoutEngine>(model, selection);
  std::println("gRParse layout: enabled ({}, {} labels, one shared session, {})",
               grparse::layout_model_name(selection), engine->labels().size(), model.string());
  return engine;
}

std::unique_ptr<TableStructureEnginePool> build_table_structure_pool(
    const std::filesystem::path& models_dir, size_t worker_count, bool layout_active) {
  const std::string mode =
      configured_mode("GRPARSE_TABLE_STRUCTURE", "auto", {"auto", "on", "off"});
  const std::filesystem::path model = models_dir / "slanet_plus.onnx";
  if (mode == "off") {
    std::println("gRParse table structure: disabled (GRPARSE_TABLE_STRUCTURE=off)");
    return nullptr;
  }
  if (!layout_active) {
    if (mode == "on") {
      throw std::invalid_argument(
          "GRPARSE_TABLE_STRUCTURE=on needs layout enabled to find table regions");
    }
    if (std::filesystem::exists(model)) {
      std::println("gRParse table structure: disabled (layout is disabled)");
    }
    return nullptr;
  }
  if (mode == "auto" && !std::filesystem::exists(model)) {
    std::println("gRParse table structure: disabled (no {}; see models/README.md)",
                 model.string());
    return nullptr;
  }
  auto pool = std::make_unique<grparse::TableStructureEnginePool>(model, worker_count);
  std::println("gRParse table structure: enabled ({} sessions, {})", pool->size(),
               model.string());
  return pool;
}

std::unique_ptr<FigureClassifierPool> build_figure_classifier_pool(
    const std::filesystem::path& models_dir, size_t worker_count, bool layout_active) {
  const std::string mode =
      configured_mode("GRPARSE_FIGURE_CLASSES", "auto", {"auto", "on", "off"});
  const std::filesystem::path model = models_dir / "figure_classifier.onnx";
  if (mode == "off") {
    std::println("gRParse figure classes: disabled (GRPARSE_FIGURE_CLASSES=off)");
    return nullptr;
  }
  if (!layout_active) {
    if (mode == "on") {
      throw std::invalid_argument(
          "GRPARSE_FIGURE_CLASSES=on needs layout enabled to find figure regions");
    }
    if (std::filesystem::exists(model)) {
      std::println("gRParse figure classes: disabled (layout is disabled)");
    }
    return nullptr;
  }
  if (mode == "auto" && !std::filesystem::exists(model)) {
    std::println("gRParse figure classes: disabled (no {}; see models/README.md)",
                 model.string());
    return nullptr;
  }
  auto pool = std::make_unique<grparse::FigureClassifierPool>(model, worker_count);
  std::println("gRParse figure classes: enabled ({} sessions, {})", pool->size(),
               model.string());
  return pool;
}

}  // namespace grparse
