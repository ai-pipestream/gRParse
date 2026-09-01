#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "grparse/data_totals.h"
#include "grparse/document_repair.h"
#include "grparse/ocr_engine.h"
#include "grparse/office_cv_enrichment.h"
#include "grparse/page_scheduler.h"

namespace grparse {

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
// standard client libraries export.  This overload is pure in its inputs.
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
