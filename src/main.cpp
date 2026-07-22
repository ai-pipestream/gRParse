#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "grparse/document_parser_service.h"
#include "grparse/page_scheduler.h"

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

// GRPARSE_ORT_EP selects the ONNX Runtime execution provider (B6).  cuda is
// the default and keeps today's fail-loud behaviour; cpu is a deliberate
// choice, never a silent fallback; auto tries CUDA and falls back to CPU with
// a logged reason.  openvino is named in the error so an Intel deployment
// learns it is planned rather than misspelled.
std::unique_ptr<grparse::OcrEnginePool> build_engine_pool(const std::filesystem::path& models,
                                                          size_t worker_count, int gpu_index) {
  const char* configured = std::getenv("GRPARSE_ORT_EP");
  const std::string ep = configured == nullptr || *configured == '\0' ? "cuda" : configured;
  if (ep == "cuda") {
    auto pool = std::make_unique<grparse::OcrEnginePool>(models, worker_count, gpu_index);
    std::cout << "gRParse OCR execution provider: CUDA (device " << gpu_index << ")" << std::endl;
    return pool;
  }
  if (ep == "cpu") {
    auto pool = std::make_unique<grparse::OcrEnginePool>(models, worker_count, -1);
    std::cout << "gRParse OCR execution provider: CPU (GRPARSE_ORT_EP=cpu)" << std::endl;
    return pool;
  }
  if (ep == "auto") {
    try {
      auto pool = std::make_unique<grparse::OcrEnginePool>(models, worker_count, gpu_index);
      std::cout << "gRParse OCR execution provider: CUDA (device " << gpu_index
                << ", selected by GRPARSE_ORT_EP=auto)" << std::endl;
      return pool;
    } catch (const std::exception& error) {
      std::cerr << "GRPARSE_ORT_EP=auto: CUDA initialization failed (" << error.what()
                << "); falling back to CPU" << std::endl;
      auto pool = std::make_unique<grparse::OcrEnginePool>(models, worker_count, -1);
      std::cout << "gRParse OCR execution provider: CPU (selected by GRPARSE_ORT_EP=auto)"
                << std::endl;
      return pool;
    }
  }
  if (ep == "openvino") {
    throw std::invalid_argument(
        "GRPARSE_ORT_EP=openvino: the OpenVINO execution provider is not compiled into this "
        "build yet (tracked as B6)");
  }
  throw std::invalid_argument("GRPARSE_ORT_EP must be cuda, cpu, or auto");
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
       << ",rendered=" << current.pages_rendered << ",ocr=" << current.pages_recognized
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
    const auto engines =
        build_engine_pool(models == nullptr ? "/models" : models, inference_workers, gpu_index);
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
    grparse::PageScheduler scheduler(*engines, options);
    grparse::DocumentParserService service(scheduler);
    grparse::DocumentStreamingService streaming_service(scheduler);
    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    builder.SetMaxReceiveMessageSize(70 * 1024 * 1024);
    grpc::ResourceQuota quota;
    quota.Resize(configured_size("GRPARSE_GRPC_MEMORY_MIB", 256, 16384) * 1024U * 1024U);
    quota.SetMaxThreads(static_cast<int>(configured_size("GRPARSE_GRPC_MAX_THREADS", 64, 1024)));
    builder.SetResourceQuota(quota);
    builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS,
                               static_cast<int>(configured_size("GRPARSE_MAX_CONCURRENT_STREAMS", 32, 1024)));
    builder.RegisterService(&service);
    builder.RegisterService(&streaming_service);
    const auto server = builder.BuildAndStart();
    if (!server) {
      std::cerr << "Unable to listen on " << listen_address << '\n';
      return 1;
    }
    std::cout << "gRParse listening on " << listen_address
              << " (RapidOCR / ONNX Runtime)" << std::endl;
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
          std::cout << format_metrics(current, previous, engines->stats(), elapsed, options)
                    << std::endl;
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
    std::cerr << "Startup failed: " << error.what() << '\n';
    return 1;
  }
}
