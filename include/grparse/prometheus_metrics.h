#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include <grpcpp/support/status.h>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "grparse/data_totals.h"
#include "grparse/document_repair.h"
#include "grparse/ocr_engine.h"
#include "grparse/office_cv_enrichment.h"
#include "grparse/page_scheduler.h"

namespace grparse {

inline constexpr std::array<std::string_view, 16> kMetricDoctypes = {
    "pdf", "docx",  "xlsx",  "pptx", "html",   "md",  "epub", "email",
    "xml", "image", "audio", "warc", "ebcdic", "csv", "txt",  "other"};

enum class DocumentCompletionStatus : size_t {
  kSuccess = 0,
  kPartialSuccess = 1,
  kFailure = 2,
  kRejected = 3,
};

inline constexpr std::array<std::string_view, 4> kDocumentCompletionStatuses = {
    "success", "partial_success", "failure", "rejected"};

inline constexpr std::array<std::string_view, 15> kMetricCollectors = {
    "libreoffice", "poi",      "calamine",      "epub",       "markup",
    "email",       "xml",      "ebcdic",        "asr",        "lol-html",
    "fastwarc",    "pdf-inspector", "grparse-cv", "confluence", "enrich"};

enum class CollectorCallStatus : size_t {
  kOk = 0,
  kError = 1,
  kTimeout = 2,
  kUnavailable = 3,
};

inline constexpr std::array<std::string_view, 4> kCollectorCallStatuses = {
    "ok", "error", "timeout", "unavailable"};

// Finite upper bounds in seconds for document and collector latency histograms.
// The final bucket in DurationHistogram::buckets is the +Inf overflow bucket.
inline constexpr std::array<double, 12> kDurationBoundsSeconds = {
    0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 30.0, 60.0};

struct DurationHistogram {
  // Non-cumulative per-bucket counts (12 finite bounds + 1 +Inf overflow).
  std::array<uint64_t, kDurationBoundsSeconds.size() + 1> buckets{};
  uint64_t sum_ns = 0;

  void observe(uint64_t duration_ns);
  uint64_t count() const;
};

struct DoctypeMetrics {
  std::array<uint64_t, kDocumentCompletionStatuses.size()> completed{};
  DurationHistogram duration{};

  uint64_t& status(DocumentCompletionStatus s) { return completed[static_cast<size_t>(s)]; }
  uint64_t status(DocumentCompletionStatus s) const { return completed[static_cast<size_t>(s)]; }
};

struct CollectorMetrics {
  std::array<uint64_t, kCollectorCallStatuses.size()> calls{};
  DurationHistogram duration{};
  uint64_t warnings = 0;

  uint64_t& status(CollectorCallStatus s) { return calls[static_cast<size_t>(s)]; }
  uint64_t status(CollectorCallStatus s) const { return calls[static_cast<size_t>(s)]; }
};

struct PipelineTelemetry {
  std::array<DoctypeMetrics, kMetricDoctypes.size()> doctypes{};
  std::array<CollectorMetrics, kMetricCollectors.size()> collectors{};

  DoctypeMetrics& doctype(std::string_view name);
  const DoctypeMetrics& doctype(std::string_view name) const;
  CollectorMetrics& collector(std::string_view name);
  const CollectorMetrics& collector(std::string_view name) const;
};

// Snapshot or reset the process-wide per-doctype and per-collector counters.
PipelineTelemetry pipeline_telemetry();
void reset_pipeline_telemetry();

void record_document_completion(std::string_view doctype, DocumentCompletionStatus status,
                                uint64_t duration_ns);
void record_collector_call(std::string_view collector, CollectorCallStatus status,
                           uint64_t duration_ns, uint64_t warnings = 0);
void record_collector_call(ai::pipestream::parse::v1::Collector collector,
                           CollectorCallStatus status, uint64_t duration_ns,
                           uint64_t warnings = 0);
void record_collector_warnings(std::string_view collector, uint64_t count = 1);
void record_collector_warnings(ai::pipestream::parse::v1::Collector collector,
                               uint64_t count = 1);

CollectorCallStatus collector_call_status_for(bool success, grpc::StatusCode code);
std::string_view metric_collector_name(ai::pipestream::parse::v1::Collector collector);
std::string_view classify_doctype(ai::pipestream::parse::v1::InputFormat format,
                                  std::string_view filename = {},
                                  std::string_view mime_type = {},
                                  std::string_view content = {});

// One Prometheus text-format exposition (version 0.0.4) of the pipeline
// counters the stdout metrics line already reports.  Counters and gauges map
// one to one; the page-latency buckets become a histogram (cumulative
// _bucket series plus _count; there is no _sum series because the scheduler
// tracks bucket counts, not a latency total - histogram_quantile() needs
// only the buckets).  Stage busy nanoseconds are exported as
// grparse_stage_busy_seconds_total next to grparse_stage_workers, so
// rate(busy_seconds[1m]) / workers is the same busy fraction the stdout line
// prints.  Orientation recovery exports as grparse_pages_rerecognized_total,
// grparse_rerecognition_passes_total and
// grparse_page_rotations_total{degrees="90"|"180"|"270"}.  The repair totals
// export as grparse_repair_changes_total{kind=...} (every RepairTotals
// counter), the office CV enrichment totals as grparse_office_cv_total{kind=...}
// and the data-plane totals as grparse_data_changes_total{kind=...}, plus the
// process_resident_memory_bytes and process_cpu_seconds_total gauges the
// standard client libraries export.  Execution-provider fallbacks export as
// grparse_ort_ep_fallbacks_total: how many sessions were rebuilt on CPU after
// their selected provider refused the graph or the device failed, and
// grparse_ort_ep_build_retries_total counts the session builds retried
// after a transient OpenVINO provider failure.  Per-doctype completions and
// latency export as grparse_documents_completed_total and
// grparse_document_duration_seconds; per-collector calls, latency, and
// warnings export as grparse_collector_calls_total,
// grparse_collector_duration_seconds, and grparse_collector_warnings_total.
// This overload is pure in its inputs.
std::string render_prometheus_metrics(const PageScheduler::Metrics& metrics,
                                      const OcrEnginePool::Stats& ocr_pool,
                                      const PageScheduler::Options& options,
                                      const RepairTotals& repairs, const DataTotals& data,
                                      const OfficeCvTotals& office_cv, uint64_t ep_fallbacks,
                                      uint64_t ep_build_retries,
                                      const PipelineTelemetry& telemetry);

// Same, with the per-doctype and per-collector telemetry read from the live
// process counters.
std::string render_prometheus_metrics(const PageScheduler::Metrics& metrics,
                                      const OcrEnginePool::Stats& ocr_pool,
                                      const PageScheduler::Options& options,
                                      const RepairTotals& repairs, const DataTotals& data,
                                      const OfficeCvTotals& office_cv, uint64_t ep_fallbacks,
                                      uint64_t ep_build_retries);

// Same, with the execution-provider fallback count read from the live process
// counter.
std::string render_prometheus_metrics(const PageScheduler::Metrics& metrics,
                                      const OcrEnginePool::Stats& ocr_pool,
                                      const PageScheduler::Options& options,
                                      const RepairTotals& repairs, const DataTotals& data,
                                      const OfficeCvTotals& office_cv);

// Same, with the office CV totals read from the live process counters.
std::string render_prometheus_metrics(const PageScheduler::Metrics& metrics,
                                      const OcrEnginePool::Stats& ocr_pool,
                                      const PageScheduler::Options& options,
                                      const RepairTotals& repairs,
                                      const DataTotals& data);

// The server's own call: data and office CV totals are the live process
// counters.
std::string render_prometheus_metrics(const PageScheduler::Metrics& metrics,
                                      const OcrEnginePool::Stats& ocr_pool,
                                      const PageScheduler::Options& options,
                                      const RepairTotals& repairs = {});

// Minimal single-threaded HTTP listener serving GET /metrics.  Deliberately
// dependency-free: a scrape endpoint needs one short-lived response per
// interval, not an HTTP framework.  Anything but GET /metrics gets 404/405,
// and a renderer exception becomes a 500 instead of taking the process down.
class MetricsHttpServer final {
 public:
  using Renderer = std::function<std::string()>;

  // Binds and serves immediately; port 0 picks a free port (see port()).
  // Throws std::runtime_error when the socket cannot be bound - a configured
  // exporter that cannot listen is a startup failure, not a warning.
  MetricsHttpServer(uint16_t port, Renderer render);
  ~MetricsHttpServer();
  MetricsHttpServer(const MetricsHttpServer&) = delete;
  MetricsHttpServer& operator=(const MetricsHttpServer&) = delete;

  uint16_t port() const { return port_; }

 private:
  void serve();

  Renderer render_;
  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};

}  // namespace grparse
