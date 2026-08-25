#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <print>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "grparse/document_parser_service.h"
#include "grparse/figure_classifier.h"
#include "grparse/layout_engine.h"
#include "grparse/page_scheduler.h"
#include "grparse/prometheus_metrics.h"
#include "grparse/table_structure_engine.h"
#include "grparse_session_ep.h"

namespace {

volatile sig_atomic_t shutdown_signal_fd = -1;

void request_shutdown(int signal_number) {
  if (shutdown_signal_fd < 0) return;
  const unsigned char signal_byte = static_cast<unsigned char>(signal_number);
  const ssize_t ignored = write(static_cast<int>(shutdown_signal_fd), &signal_byte,
                                sizeof(signal_byte));
  (void)ignored;
}

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
  // GPU, GPU.1, CPU, NPU, AUTO:GPU,CPU, HETERO:… — validate the alphabet and
  // let OpenVINO reject unknown devices itself, loudly, at startup.
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
// the RapidOcr hook actually ran — a dependency tree built without
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

// GRPARSE_ORT_EP selects the ONNX Runtime execution provider (B6).  cuda is
// the default and keeps the fail-loud behaviour; cpu is a deliberate choice,
// never a silent fallback; openvino targets Intel GPUs/NPUs through the
// OpenVINO build of ONNX Runtime; auto prefers CUDA, then OpenVINO, then CPU,
// logging each fallback.  Requesting a provider this binary was not built
// with fails with the list that is actually available.
std::unique_ptr<grparse::OcrEnginePool> build_engine_pool(const std::filesystem::path& models,
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

// GRPARSE_LAYOUT: auto (default) enables layout labelling when the model file
// exists; on requires it and fails startup when absent; off disables it.
// GRPARSE_LAYOUT_MODEL picks which detector that file is.  Nothing here
// degrades silently: auto logs which way it went, and an explicitly selected
// model that is not on disk stops the process.
std::unique_ptr<grparse::LayoutEngine> build_layout_engine(
    const std::filesystem::path& models_dir) {
  const char* configured = std::getenv("GRPARSE_LAYOUT");
  const std::string mode = configured == nullptr || *configured == '\0' ? "auto" : configured;
  if (mode != "auto" && mode != "on" && mode != "off") {
    throw std::invalid_argument("GRPARSE_LAYOUT must be auto, on, or off");
  }
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

// GRPARSE_TABLE_STRUCTURE follows the same auto/on/off contract as layout.
// Structure only ever sees crops of layout-detected table regions, so it
// additionally requires layout to be active.
std::unique_ptr<grparse::TableStructureEnginePool> build_table_structure_pool(
    const std::filesystem::path& models_dir, size_t worker_count, bool layout_active) {
  const char* configured = std::getenv("GRPARSE_TABLE_STRUCTURE");
  const std::string mode = configured == nullptr || *configured == '\0' ? "auto" : configured;
  if (mode != "auto" && mode != "on" && mode != "off") {
    throw std::invalid_argument("GRPARSE_TABLE_STRUCTURE must be auto, on, or off");
  }
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

// GRPARSE_FIGURE_CLASSES follows the same auto/on/off contract; the
// classifier only ever sees crops of layout-detected figure regions.
std::unique_ptr<grparse::FigureClassifierPool> build_figure_classifier_pool(
    const std::filesystem::path& models_dir, size_t worker_count, bool layout_active) {
  const char* configured = std::getenv("GRPARSE_FIGURE_CLASSES");
  const std::string mode = configured == nullptr || *configured == '\0' ? "auto" : configured;
  if (mode != "auto" && mode != "on" && mode != "off") {
    throw std::invalid_argument("GRPARSE_FIGURE_CLASSES must be auto, on, or off");
  }
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

// GRPARSE_BARCODES: auto (default) decodes figure crops whose top classifier
// call is bar_code or qr_code, so it needs the classifier; on decodes every
// figure crop (needs only layout); off disables decoding.  ZXing is compiled
// in, so no model file gates this.
grparse::PageScheduler::BarcodeMode configure_barcode_mode(bool layout_active,
                                                           bool classifier_active) {
  using BarcodeMode = grparse::PageScheduler::BarcodeMode;
  const char* configured = std::getenv("GRPARSE_BARCODES");
  const std::string mode = configured == nullptr || *configured == '\0' ? "auto" : configured;
  if (mode != "auto" && mode != "on" && mode != "off") {
    throw std::invalid_argument("GRPARSE_BARCODES must be auto, on, or off");
  }
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

size_t page_worker_count() {
  const unsigned int hardware = std::thread::hardware_concurrency();
  return std::min<size_t>(2, hardware == 0 ? 1 : hardware);
}

unsigned busy_percent(uint64_t busy_ns_delta, double elapsed_seconds, size_t workers) {
  if (elapsed_seconds <= 0.0 || workers == 0) return 0;
  const double fraction =
      static_cast<double>(busy_ns_delta) / (elapsed_seconds * 1e9 * static_cast<double>(workers));
  return static_cast<unsigned>(std::min(100.0, fraction * 100.0));
}

// One line per interval, deltas where rates matter and totals where they
// don't.  Render and inference busy% climbing together under load is the
// anti-seesaw doctrine holding; inference pegged while render idles (or the
// reverse) says which stage to give workers.
std::string format_metrics(const grparse::PageScheduler::Metrics& current,
                           const grparse::PageScheduler::Metrics& previous,
                           const grparse::OcrEnginePool::Stats& ocr, double elapsed_seconds,
                           const grparse::PageScheduler::Options& options) {
  std::ostringstream line;
  line << "gRParse metrics:"
       << " docs{submitted=" << current.documents_submitted
       << ",rejected=" << current.documents_rejected << ",queued=" << current.documents_queued
       << "}"
       << " pages{digital=" << current.pages_read_digitally
       << ",rendered=" << current.pages_rendered << ",ocr=" << current.pages_recognized << ",layout=" << current.pages_layout_labelled
       << ",tables=" << current.tables_structured
       << ",figures=" << current.figures_classified
       << ",barcodes=" << current.barcodes_decoded
       << ",cancelled=" << current.pages_cancelled << "}"
       << " queues{render=" << current.pages_waiting_for_render
       << ",inference=" << current.pages_waiting_for_inference
       << ",assembly=" << current.pages_waiting_for_assembly << "}"
       << " busy%{render="
       << busy_percent(current.render_busy_ns - previous.render_busy_ns, elapsed_seconds,
                       options.render_workers)
       << ",inference="
       << busy_percent(current.inference_busy_ns - previous.inference_busy_ns, elapsed_seconds,
                       options.inference_workers)
       << ",assembly="
       << busy_percent(current.assembly_busy_ns - previous.assembly_busy_ns, elapsed_seconds,
                       options.assembly_workers)
       << "}"
       << " ocr_pool{acquires=" << ocr.acquires << ",discards=" << ocr.discards
       << ",wait_ms=" << ocr.wait_ns / 1000000 << "}";
  line << " latency_ms{";
  for (size_t bucket = 0; bucket < current.page_latency.size(); ++bucket) {
    if (bucket > 0) line << ",";
    if (bucket < grparse::PageScheduler::kPageLatencyBoundsMs.size()) {
      line << "<=" << grparse::PageScheduler::kPageLatencyBoundsMs[bucket];
    } else {
      line << ">" << grparse::PageScheduler::kPageLatencyBoundsMs.back();
    }
    line << ":" << current.page_latency[bucket];
  }
  line << "}";
  return line.str();
}

void install_shutdown_signal_pipe(int* signal_pipe) {
  if (pipe2(signal_pipe, O_CLOEXEC) != 0) {
    throw std::runtime_error("Could not create graceful shutdown signal pipe");
  }
  shutdown_signal_fd = signal_pipe[1];
  struct sigaction action {};
  action.sa_handler = request_shutdown;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) {
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    shutdown_signal_fd = -1;
    throw std::runtime_error("Could not install graceful shutdown signal handlers");
  }
}

}  // namespace

int main() {
  // Container stdout is a pipe (fully buffered); line-buffer it explicitly so
  // the old endl-flushing behaviour survives println, which does not flush.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  const char* models = std::getenv("GRPARSE_MODELS_DIR");
  const char* address = std::getenv("GRPARSE_LISTEN_ADDRESS");
  const std::string listen_address = address == nullptr ? "0.0.0.0:50051" : address;
  try {
    int signal_pipe[2];
    install_shutdown_signal_pipe(signal_pipe);
    const size_t inference_workers = configured_size("GRPARSE_PAGE_WORKERS", page_worker_count(), 64);
    const unsigned int hardware = std::thread::hardware_concurrency();
    const size_t render_workers = configured_size(
        "GRPARSE_RENDER_WORKERS", std::min<size_t>(4, hardware == 0 ? 2 : hardware), 256);
    const int gpu_index = configured_index("GRPARSE_CUDA_DEVICE", 0);
    const std::filesystem::path models_dir = models == nullptr ? "/models" : models;
    const auto engines = build_engine_pool(models_dir, inference_workers, gpu_index);
    const auto layout = build_layout_engine(models_dir);
    const auto table_structure =
        build_table_structure_pool(models_dir, inference_workers, layout != nullptr);
    const auto figure_classes =
        build_figure_classifier_pool(models_dir, inference_workers, layout != nullptr);
    // Named assignment on purpose: a positional brace list of nine same-typed
    // sizes is one reordering away from a silent misconfiguration.
    grparse::PageScheduler::Options options;
    options.document_queue_capacity = configured_size("GRPARSE_DOCUMENT_QUEUE", 8);
    options.render_queue_capacity = configured_size("GRPARSE_RENDER_QUEUE", 8);
    options.inference_queue_capacity = configured_size("GRPARSE_INFERENCE_QUEUE", 4);
    options.assembly_queue_capacity = configured_size("GRPARSE_ASSEMBLY_QUEUE", 8);
    options.render_workers = render_workers;
    options.inference_workers = inference_workers;
    options.assembly_workers = configured_size("GRPARSE_ASSEMBLY_WORKERS", 2, 64);
    options.page_window = configured_size("GRPARSE_PAGE_WINDOW", 4, 64);
    options.max_active_documents = configured_size("GRPARSE_MAX_ACTIVE_DOCUMENTS", 32, 1024);
    options.pdf_parsers = configured_size("GRPARSE_PDF_PARSERS", render_workers, 256);
    // GRPARSE_PICTURE_IMAGES=on embeds PNG crops of figure regions in picture
    // items.  Off by default: crops inflate page events on figure-heavy docs.
    const char* picture_images = std::getenv("GRPARSE_PICTURE_IMAGES");
    const std::string picture_mode =
        picture_images == nullptr || *picture_images == '\0' ? "off" : picture_images;
    if (picture_mode != "on" && picture_mode != "off") {
      throw std::invalid_argument("GRPARSE_PICTURE_IMAGES must be on or off");
    }
    options.capture_picture_images = picture_mode == "on" && layout != nullptr;
    if (picture_mode == "on" && layout == nullptr) {
      std::println("gRParse picture images: disabled (layout is disabled)");
    } else if (options.capture_picture_images) {
      std::println("gRParse picture images: enabled");
    }
    // GRPARSE_PAGE_IMAGES=on embeds a downscaled PNG preview of every page
    // raster in the page event, so clients can paint boxes over the real
    // page.  Off by default: previews add bytes to every page event.  Unlike
    // picture images this needs no layout model; it forces rasterization of
    // full-digital pages instead.
    const char* page_images = std::getenv("GRPARSE_PAGE_IMAGES");
    const std::string page_image_mode =
        page_images == nullptr || *page_images == '\0' ? "off" : page_images;
    if (page_image_mode != "on" && page_image_mode != "off") {
      throw std::invalid_argument("GRPARSE_PAGE_IMAGES must be on or off");
    }
    options.capture_page_images = page_image_mode == "on";
    if (options.capture_page_images) {
      std::println("gRParse page images: enabled");
    }
    options.barcode_mode = configure_barcode_mode(layout != nullptr, figure_classes != nullptr);
    grparse::PageScheduler scheduler(*engines, options, grparse::PageSourceFactory{},
                                     layout.get(), table_structure.get(),
                                     figure_classes.get());
    // GRPARSE_<COLLECTOR>_TARGET names each remote collector's endpoint.
    // Unset leaves that collector unconfigured: documents routed to it then
    // fail that collector with a clear error instead of being converted
    // through any intermediate. GRPARSE_ASR_MODEL names the whisper model
    // grpc-asr must serve; the asr wire requires one.
    const auto collector_env = [](const char* name) {
      const char* configured = std::getenv(name);
      return configured == nullptr ? std::string() : std::string(configured);
    };
    grparse::CollectorTargets targets{
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
    };
    // The hybrid leg: office documents' page renders run through the same
    // layout/classifier/barcode engines the CV path uses, sharing its pools.
    grparse::OfficeCvEnrichment office_cv{
        .detector = layout.get(),
        .classifier = figure_classes.get(),
        .barcode_mode = options.barcode_mode,
    };
    const auto endpoints =
        std::make_shared<grparse::CollectorEndpoints>(targets, office_cv);
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
    if (!targets.libreoffice.empty()) {
      std::println("gRParse office CV enrichment: {}",
                   layout != nullptr ? "enabled (layout"
                                       + std::string(figure_classes != nullptr
                                                         ? " + figure classes"
                                                         : "")
                                       + ")"
                                     : "disabled (layout is disabled)");
    }
    grparse::DocumentParserService service(scheduler, endpoints);
    grparse::DocumentStreamingService streaming_service(scheduler, endpoints);
    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    builder.SetMaxReceiveMessageSize(520 * 1024 * 1024);
    grpc::ResourceQuota quota;
    quota.Resize(configured_size("GRPARSE_GRPC_MEMORY_MIB", 640, 16384) * 1024U * 1024U);
    quota.SetMaxThreads(static_cast<int>(configured_size("GRPARSE_GRPC_MAX_THREADS", 64, 1024)));
    builder.SetResourceQuota(quota);
    builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS,
                               static_cast<int>(configured_size("GRPARSE_MAX_CONCURRENT_STREAMS", 32, 1024)));
    builder.RegisterService(&service);
    builder.RegisterService(&streaming_service);
    const auto server = builder.BuildAndStart();
    if (!server) {
      std::println(stderr, "Unable to listen on {}", listen_address);
      return 1;
    }
    std::println("gRParse listening on {} (RapidOCR / ONNX Runtime)", listen_address);
    // GRPARSE_METRICS_PORT exposes the same counters in Prometheus text
    // format at /metrics.  0 (the default) keeps the listener off; a
    // configured port that cannot be bound fails startup loudly.
    const int metrics_port = configured_index("GRPARSE_METRICS_PORT", 0, 65535);
    std::unique_ptr<grparse::MetricsHttpServer> metrics_exporter;
    if (metrics_port > 0) {
      metrics_exporter = std::make_unique<grparse::MetricsHttpServer>(
          static_cast<uint16_t>(metrics_port), [&scheduler, &engines, &options] {
            return grparse::render_prometheus_metrics(scheduler.metrics(), engines->stats(),
                                                      options);
          });
      std::println("gRParse metrics exporter: http://0.0.0.0:{}/metrics",
                   metrics_exporter->port());
    }
    // Pipeline visibility (B4): one metrics line per interval on stdout, where
    // container logging already looks.  0 disables.
    const int metrics_interval = configured_index("GRPARSE_METRICS_INTERVAL_SECONDS", 60, 86400);
    std::mutex metrics_mutex;
    std::condition_variable metrics_stop_changed;
    bool metrics_stop = false;
    std::thread metrics_thread;
    if (metrics_interval > 0) {
      metrics_thread = std::thread([&] {
        auto previous = scheduler.metrics();
        auto previous_time = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(metrics_mutex);
        while (!metrics_stop_changed.wait_for(lock, std::chrono::seconds(metrics_interval),
                                              [&] { return metrics_stop; })) {
          lock.unlock();
          const auto current = scheduler.metrics();
          const auto now = std::chrono::steady_clock::now();
          const double elapsed = std::chrono::duration<double>(now - previous_time).count();
          std::println("{}", format_metrics(current, previous, engines->stats(), elapsed, options));
          previous = current;
          previous_time = now;
          lock.lock();
        }
      });
    }
    std::atomic<bool> serving{true};
    std::thread shutdown_thread([&] {
      unsigned char received_signal = 0;
      if (read(signal_pipe[0], &received_signal, sizeof(received_signal)) ==
              static_cast<ssize_t>(sizeof(received_signal)) &&
          serving.load()) {
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(10));
      }
    });
    server->Wait();
    if (metrics_thread.joinable()) {
      {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        metrics_stop = true;
      }
      metrics_stop_changed.notify_all();
      metrics_thread.join();
    }
    // Wake the reader even when Wait() returned without a signal, so join()
    // cannot hang on a blocking read that will never be satisfied.
    serving.store(false);
    const unsigned char wakeup = 0;
    if (write(signal_pipe[1], &wakeup, sizeof(wakeup)) < 0) {
      // The reader has already exited; nothing left to wake.
    }
    shutdown_thread.join();
    shutdown_signal_fd = -1;
    close(signal_pipe[0]);
    close(signal_pipe[1]);
  } catch (const std::exception& error) {
    std::println(stderr, "Startup failed: {}", error.what());
    return 1;
  }
}
