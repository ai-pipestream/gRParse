#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "grparse/document_parser_service.h"
#include "grparse/office_cv_enrichment.h"
#include "grparse/page_scheduler.h"
#include "grparse/prometheus_metrics.h"
#include "grparse_session_ep.h"
#include "server_config.h"

namespace {

volatile sig_atomic_t shutdown_signal_fd = -1;

void request_shutdown(int signal_number) {
  if (shutdown_signal_fd < 0) return;
  const unsigned char signal_byte = static_cast<unsigned char>(signal_number);
  const ssize_t ignored = write(static_cast<int>(shutdown_signal_fd), &signal_byte,
                                sizeof(signal_byte));
  (void)ignored;
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
                           const grparse::PageScheduler::Options& options,
                           const grparse::RepairTotals& repairs,
                           const grparse::OfficeCvTotals& office_cv) {
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
       << " rotation{rerecognized=" << current.pages_rerecognized
       << ",passes=" << current.rerecognition_passes;
  for (size_t turn = 0; turn < grparse::PageScheduler::kRotationDegrees.size(); ++turn) {
    line << "," << grparse::PageScheduler::kRotationDegrees[turn] << "="
         << current.rotations_applied[turn];
  }
  line << "}"
       << " repairs{furniture=" << repairs.furniture_demoted
       << ",hyphens=" << repairs.hyphens_rejoined << ",paragraphs=" << repairs.paragraphs_merged
       << ",titles=" << repairs.titles_merged << ",levels=" << repairs.heading_levels_assigned
       << ",reordered=" << repairs.body_items_reordered << ",splits=" << repairs.headings_split
       << ",demoted=" << repairs.headings_demoted << ",form_rows=" << repairs.form_rows_split
       << "}"
       << " office_cv{added=" << office_cv.pictures_added
       << ",anchored=" << office_cv.pictures_anchored << "}"
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

// The interval line's own bookkeeping: rates are deltas, so the previous
// sample and the moment it was taken belong to the reporter, not the caller.
class MetricsLine {
 public:
  MetricsLine(const grparse::PageScheduler& scheduler, const grparse::OcrEnginePool& engines,
              const grparse::PageScheduler::Options& options)
      : scheduler_(scheduler),
        engines_(engines),
        options_(options),
        previous_(scheduler.metrics()),
        previous_time_(std::chrono::steady_clock::now()) {}

  std::string next() {
    const auto current = scheduler_.metrics();
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - previous_time_).count();
    std::string line = format_metrics(current, previous_, engines_.stats(), elapsed, options_,
                                      grparse::repair_totals(), grparse::office_cv_totals());
    previous_ = current;
    previous_time_ = now;
    return line;
  }

 private:
  const grparse::PageScheduler& scheduler_;
  const grparse::OcrEnginePool& engines_;
  const grparse::PageScheduler::Options& options_;
  grparse::PageScheduler::Metrics previous_;
  std::chrono::steady_clock::time_point previous_time_;
};

// The metrics line on its own thread, one line per interval until stopped.
// An interval of 0 starts no thread, which is what disables the line.
class MetricsLogLoop {
 public:
  MetricsLogLoop(int interval_seconds, std::function<std::string()> line) {
    if (interval_seconds <= 0) return;
    thread_ = std::thread([this, interval_seconds, line = std::move(line)] {
      std::unique_lock<std::mutex> lock(mutex_);
      while (!stop_changed_.wait_for(lock, std::chrono::seconds(interval_seconds),
                                     [this] { return stop_; })) {
        lock.unlock();
        std::println("{}", line());
        lock.lock();
      }
    });
  }

  MetricsLogLoop(const MetricsLogLoop&) = delete;
  MetricsLogLoop& operator=(const MetricsLogLoop&) = delete;

  ~MetricsLogLoop() { stop(); }

  void stop() {
    if (!thread_.joinable()) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    stop_changed_.notify_all();
    thread_.join();
  }

 private:
  std::mutex mutex_;
  std::condition_variable stop_changed_;
  bool stop_ = false;
  std::thread thread_;
};

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

// Binds the port and registers both parsing surfaces. Null when the address
// could not be listened on, which the caller reports and exits over.
std::unique_ptr<grpc::Server> start_server(const std::string& listen_address,
                                           grparse::DocumentParserService& service,
                                           grparse::DocumentStreamingService& streaming_service,
                                           const grparse::GrpcLimits& limits) {
  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
  builder.SetMaxReceiveMessageSize(grparse::kMaxMessageBytes);
  grpc::ResourceQuota quota;
  quota.Resize(limits.memory_mib * 1024U * 1024U);
  quota.SetMaxThreads(static_cast<int>(limits.max_threads));
  builder.SetResourceQuota(quota);
  builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS,
                             static_cast<int>(limits.max_concurrent_streams));
  builder.RegisterService(&service);
  builder.RegisterService(&streaming_service);
  return builder.BuildAndStart();
}

std::unique_ptr<grparse::MetricsHttpServer> start_metrics_exporter(
    int port, const grparse::PageScheduler& scheduler, const grparse::OcrEnginePool& engines,
    const grparse::PageScheduler::Options& options) {
  if (port <= 0) return nullptr;
  auto exporter = std::make_unique<grparse::MetricsHttpServer>(
      static_cast<uint16_t>(port), [&scheduler, &engines, &options] {
        return grparse::render_prometheus_metrics(scheduler.metrics(), engines.stats(), options,
                                                  grparse::repair_totals());
      });
  std::println("gRParse metrics exporter: http://0.0.0.0:{}/metrics", exporter->port());
  return exporter;
}

// Serves until SIGINT or SIGTERM arrives on the signal pipe, then drains:
// the interval line stops first, then the pipe's reader is woken even when
// Wait() returned without a signal, so join() cannot hang on a read nothing
// will satisfy.
void serve_until_signal(grpc::Server& server, const int* signal_pipe, MetricsLogLoop& metrics) {
  std::atomic<bool> serving{true};
  std::thread shutdown_thread([&] {
    unsigned char received_signal = 0;
    if (read(signal_pipe[0], &received_signal, sizeof(received_signal)) ==
            static_cast<ssize_t>(sizeof(received_signal)) &&
        serving.load()) {
      server.Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(10));
    }
  });
  server.Wait();
  metrics.stop();
  serving.store(false);
  const unsigned char wakeup = 0;
  if (write(signal_pipe[1], &wakeup, sizeof(wakeup)) < 0) {
    // The reader has already exited; nothing left to wake.
  }
  shutdown_thread.join();
  shutdown_signal_fd = -1;
  close(signal_pipe[0]);
  close(signal_pipe[1]);
}

}  // namespace

int main() {
  // Container stdout is a pipe (fully buffered); line-buffer it explicitly so
  // the old endl-flushing behaviour survives println, which does not flush.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  const grparse::ProcessConfig process = grparse::read_process_config();
  try {
    int signal_pipe[2];
    install_shutdown_signal_pipe(signal_pipe);
    const grparse::WorkerConfig workers = grparse::read_worker_config();
    grparse::set_ort_intra_op_threads(workers.intra_op_threads);
    std::println("gRParse inference threads: {} per pooled session, {} workers, {} cores",
                 workers.intra_op_threads, workers.inference_workers, workers.cores);
    const auto engines =
        grparse::build_engine_pool(process.models_dir, workers.inference_workers, workers.gpu_index);
    const auto layout = grparse::build_layout_engine(process.models_dir);
    const auto table_structure = grparse::build_table_structure_pool(
        process.models_dir, workers.inference_workers, layout != nullptr);
    const auto figure_classes = grparse::build_figure_classifier_pool(
        process.models_dir, workers.inference_workers, layout != nullptr);
    const grparse::PageScheduler::Options options =
        grparse::read_scheduler_options(workers, layout != nullptr, figure_classes != nullptr);
    grparse::PageScheduler scheduler(*engines, options, grparse::PageSourceFactory{},
                                     layout.get(), table_structure.get(), figure_classes.get());
    const grparse::CollectorTargets targets = grparse::read_collector_targets();
    // The hybrid leg: office documents' page renders run through the same
    // layout/classifier/barcode engines the CV path uses, sharing its pools.
    const grparse::OfficeCvEnrichment office_cv{
        .detector = layout.get(),
        .classifier = figure_classes.get(),
        .barcode_mode = options.barcode_mode,
    };
    const auto endpoints = std::make_shared<grparse::CollectorEndpoints>(targets, office_cv);
    grparse::report_collector_targets(targets, layout != nullptr, figure_classes != nullptr);
    const grparse::CallExecutor::Options executor_options = grparse::read_executor_options();
    std::println("gRParse unary executor: {} workers, queue {}", executor_options.workers,
                 executor_options.queue_capacity);
    const std::optional<grparse::RepairOptions> repair = grparse::configure_repair();
    grparse::DocumentParserService service(scheduler, endpoints, executor_options, repair);
    grparse::DocumentStreamingService streaming_service(scheduler, endpoints, repair);
    const auto server = start_server(process.listen_address, service, streaming_service,
                                     grparse::read_grpc_limits());
    if (!server) {
      std::println(stderr, "Unable to listen on {}", process.listen_address);
      return 1;
    }
    std::println("gRParse listening on {} (RapidOCR / ONNX Runtime)", process.listen_address);
    const grparse::MetricsConfig metrics_config = grparse::read_metrics_config();
    const auto metrics_exporter =
        start_metrics_exporter(metrics_config.port, scheduler, *engines, options);
    MetricsLine metrics_line(scheduler, *engines, options);
    MetricsLogLoop metrics_log(metrics_config.interval_seconds,
                               [&metrics_line] { return metrics_line.next(); });
    serve_until_signal(*server, signal_pipe, metrics_log);
  } catch (const std::exception& error) {
    std::println(stderr, "Startup failed: {}", error.what());
    return 1;
  }
}
