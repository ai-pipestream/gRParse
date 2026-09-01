// The render function is pure, so its output is asserted exactly; the HTTP
// listener is exercised over a real loopback socket (port 0) to prove the
// request routing, the content type, and the failure doors.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "grparse/prometheus_metrics.h"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_contains(const std::string& haystack, const std::string& needle,
                      const std::string& message) {
  require(haystack.contains(needle), message + " (missing: " + needle + ")");
}

std::string http_exchange(uint16_t port, const std::string& request) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  require(fd >= 0, "client socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  require(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
          "connect to metrics listener");
  size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t wrote = ::send(fd, request.data() + sent, request.size() - sent, 0);
    require(wrote > 0, "send request");
    sent += static_cast<size_t>(wrote);
  }
  ::shutdown(fd, SHUT_WR);
  std::string response;
  char buffer[1024];
  ssize_t received = 0;
  while ((received = ::recv(fd, buffer, sizeof(buffer), 0)) > 0) {
    response.append(buffer, static_cast<size_t>(received));
  }
  ::close(fd);
  return response;
}

void verify_render_counters_and_gauges() {
  grparse::PageScheduler::Metrics metrics;
  metrics.documents_submitted = 7;
  metrics.documents_rejected = 2;
  metrics.documents_queued = 3;
  metrics.pages_read_digitally = 11;
  metrics.pages_rendered = 13;
  metrics.pages_recognized = 5;
  metrics.pages_layout_labelled = 4;
  metrics.tables_structured = 1;
  metrics.figures_classified = 6;
  metrics.barcodes_decoded = 2;
  metrics.pages_cancelled = 1;
  metrics.pages_waiting_for_render = 8;
  metrics.pages_waiting_for_inference = 9;
  metrics.pages_waiting_for_assembly = 10;
  metrics.render_busy_ns = 1500000000;  // 1.5 s
  metrics.inference_busy_ns = 250000000;
  metrics.assembly_busy_ns = 0;

  grparse::OcrEnginePool::Stats ocr;
  ocr.acquires = 40;
  ocr.discards = 1;
  ocr.wait_ns = 2000000000;  // 2 s

  grparse::PageScheduler::Options options{
      .render_workers = 4,
      .inference_workers = 2,
      .assembly_workers = 1,
  };

  grparse::DataTotals data;
  data.charts_bound = 4;
  data.sheet_header_rows = 2;
  data.mimetypes_sniffed = 9;
  data.charts_derendered = 2;
  data.chart_derender_skipped = 1;
  const std::string text =
      grparse::render_prometheus_metrics(metrics, ocr, options, grparse::RepairTotals{}, data);
  require_contains(text, "grparse_documents_submitted_total 7\n", "submitted counter");
  require_contains(text, "# TYPE grparse_data_changes_total counter\n", "data totals type line");
  require_contains(text, "grparse_data_changes_total{kind=\"charts_bound\"} 4\n",
                   "charts bound counter");
  require_contains(text, "grparse_data_changes_total{kind=\"sheet_header_rows\"} 2\n",
                   "sheet header rows counter");
  require_contains(text, "grparse_data_changes_total{kind=\"mimetypes_sniffed\"} 9\n",
                   "sniffed mimetypes counter");
  require_contains(text, "grparse_data_changes_total{kind=\"cv_enrichment_skipped\"} 0\n",
                   "untouched data counters read zero");
  require_contains(text, "grparse_data_changes_total{kind=\"charts_derendered\"} 2\n",
                   "charts_derendered is exported");
  require_contains(text, "grparse_data_changes_total{kind=\"chart_derender_skipped\"} 1\n",
                   "chart_derender_skipped is exported");
  require_contains(text, "# TYPE process_resident_memory_bytes gauge\n", "resident memory gauge");
  require_contains(text, "# TYPE process_cpu_seconds_total counter\n", "cpu seconds counter");
  const auto rss_at = text.find("\nprocess_resident_memory_bytes ");
  require(rss_at != std::string::npos &&
              std::stoull(text.substr(rss_at + 31)) > 0,
          "a running process has a resident set");
  require_contains(text, "grparse_documents_rejected_total 2\n", "rejected counter");
  require_contains(text, "grparse_documents_queued 3\n", "queued gauge");
  require_contains(text, "grparse_pages_digital_total 11\n", "digital pages counter");
  require_contains(text, "grparse_pages_rendered_total 13\n", "rendered pages counter");
  require_contains(text, "grparse_pages_ocr_total 5\n", "ocr pages counter");
  require_contains(text, "grparse_pages_layout_total 4\n", "layout pages counter");
  require_contains(text, "grparse_tables_structured_total 1\n", "tables counter");
  require_contains(text, "grparse_figures_classified_total 6\n", "figures counter");
  require_contains(text, "grparse_barcodes_decoded_total 2\n", "barcodes counter");
  require_contains(text, "grparse_pages_cancelled_total 1\n", "cancelled counter");
  require_contains(text, "grparse_pages_waiting{stage=\"render\"} 8\n", "render queue gauge");
  require_contains(text, "grparse_pages_waiting{stage=\"inference\"} 9\n", "inference queue gauge");
  require_contains(text, "grparse_pages_waiting{stage=\"assembly\"} 10\n", "assembly queue gauge");
  require_contains(text, "grparse_stage_busy_seconds_total{stage=\"render\"} 1.5\n",
                   "busy nanoseconds convert to seconds");
  require_contains(text, "grparse_stage_busy_seconds_total{stage=\"inference\"} 0.25\n",
                   "inference busy seconds");
  require_contains(text, "grparse_stage_workers{stage=\"render\"} 4\n", "render workers gauge");
  require_contains(text, "grparse_stage_workers{stage=\"inference\"} 2\n",
                   "inference workers gauge");
  require_contains(text, "grparse_ocr_pool_acquires_total 40\n", "pool acquires");
  require_contains(text, "grparse_ocr_pool_discards_total 1\n", "pool discards");
  require_contains(text, "grparse_ocr_pool_wait_seconds_total 2\n", "pool wait seconds");
  require_contains(text, "# TYPE grparse_page_latency_seconds histogram\n", "histogram type");
}

// The orientation counters, every repair counter, and the office CV totals,
// as families a dashboard can select by label.
void verify_render_rotation_repair_and_office_cv_families() {
  grparse::PageScheduler::Metrics metrics;
  metrics.pages_rerecognized = 3;
  metrics.rerecognition_passes = 5;
  metrics.rotations_applied = {1, 0, 2};

  grparse::RepairTotals repairs;
  repairs.furniture_demoted = 11;
  repairs.hyphens_rejoined = 12;
  repairs.paragraphs_merged = 13;
  repairs.titles_merged = 14;
  repairs.heading_levels_assigned = 15;
  repairs.body_items_reordered = 16;
  repairs.headings_split = 17;
  repairs.headings_demoted = 18;
  repairs.form_rows_split = 19;

  grparse::OfficeCvTotals office_cv;
  office_cv.pictures_added = 21;
  office_cv.pictures_anchored = 20;

  const std::string text = grparse::render_prometheus_metrics(
      metrics, grparse::OcrEnginePool::Stats{}, grparse::PageScheduler::Options{}, repairs,
      grparse::DataTotals{}, office_cv);
  require_contains(text, "# TYPE grparse_pages_rerecognized_total counter\n",
                   "rerecognized pages type line");
  require_contains(text, "grparse_pages_rerecognized_total 3\n", "rerecognized pages counter");
  require_contains(text, "grparse_rerecognition_passes_total 5\n", "rerecognition passes counter");
  require_contains(text, "# TYPE grparse_page_rotations_total counter\n", "rotations type line");
  require_contains(text, "grparse_page_rotations_total{degrees=\"90\"} 1\n", "90 degree turns");
  require_contains(text, "grparse_page_rotations_total{degrees=\"180\"} 0\n", "180 degree turns");
  require_contains(text, "grparse_page_rotations_total{degrees=\"270\"} 2\n", "270 degree turns");

  require_contains(text, "# TYPE grparse_repair_changes_total counter\n", "repairs type line");
  const std::vector<std::pair<const char*, int>> kinds = {
      {"furniture_demoted", 11}, {"hyphens_rejoined", 12},       {"paragraphs_merged", 13},
      {"titles_merged", 14},     {"heading_levels_assigned", 15}, {"body_items_reordered", 16},
      {"headings_split", 17},    {"headings_demoted", 18},        {"form_rows_split", 19}};
  for (const auto& [kind, value] : kinds) {
    require_contains(text,
                     "grparse_repair_changes_total{kind=\"" + std::string(kind) + "\"} " +
                         std::to_string(value) + "\n",
                     std::string("repair counter ") + kind);
  }
  require(!text.contains("grparse_repairs_total"), "the old repair family name is gone");

  require_contains(text, "# TYPE grparse_office_cv_total counter\n", "office cv type line");
  require_contains(text, "grparse_office_cv_total{kind=\"pictures_added\"} 21\n",
                   "office cv pictures added");
  require_contains(text, "grparse_office_cv_total{kind=\"pictures_anchored\"} 20\n",
                   "office cv pictures anchored");

  // The live-totals overload still renders the families, at their process
  // values (zero in a test binary that enriched nothing).
  const std::string live = grparse::render_prometheus_metrics(
      metrics, grparse::OcrEnginePool::Stats{}, grparse::PageScheduler::Options{});
  require_contains(live, "grparse_office_cv_total{kind=\"pictures_added\"} 0\n",
                   "live office cv totals start at zero");
  require_contains(live, "grparse_repair_changes_total{kind=\"form_rows_split\"} 0\n",
                   "live repair totals start at zero");
}

void verify_render_histogram_is_cumulative() {
  grparse::PageScheduler::Metrics metrics;
  metrics.page_latency[0] = 1;  // <= 25 ms
  metrics.page_latency[2] = 2;  // <= 100 ms
  metrics.page_latency[metrics.page_latency.size() - 1] = 3;  // overflow bucket

  const std::string text = grparse::render_prometheus_metrics(
      metrics, grparse::OcrEnginePool::Stats{}, grparse::PageScheduler::Options{});
  require_contains(text, "grparse_page_latency_seconds_bucket{le=\"0.025\"} 1\n",
                   "first bucket in seconds");
  require_contains(text, "grparse_page_latency_seconds_bucket{le=\"0.05\"} 1\n",
                   "empty bucket carries the running total");
  require_contains(text, "grparse_page_latency_seconds_bucket{le=\"0.1\"} 3\n",
                   "buckets accumulate");
  require_contains(text, "grparse_page_latency_seconds_bucket{le=\"10\"} 3\n",
                   "last bounded bucket");
  require_contains(text, "grparse_page_latency_seconds_bucket{le=\"+Inf\"} 6\n",
                   "+Inf bucket counts everything");
  require_contains(text, "grparse_page_latency_seconds_count 6\n", "histogram count");
}

void verify_http_listener() {
  grparse::MetricsHttpServer server(0, [] { return std::string("test_metric 1\n"); });
  require(server.port() != 0, "port 0 must resolve to a bound port");

  const std::string ok = http_exchange(
      server.port(), "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  require_contains(ok, "HTTP/1.1 200 OK", "scrape returns 200");
  require_contains(ok, "text/plain; version=0.0.4", "prometheus content type");
  require_contains(ok, "test_metric 1\n", "scrape returns the rendered body");

  const std::string not_found =
      http_exchange(server.port(), "GET /other HTTP/1.1\r\nHost: localhost\r\n\r\n");
  require_contains(not_found, "HTTP/1.1 404 Not Found", "unknown path returns 404");

  const std::string bad_method =
      http_exchange(server.port(), "POST /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  require_contains(bad_method, "HTTP/1.1 405 Method Not Allowed", "non-GET returns 405");

  // The listener survives a scrape and keeps serving.
  const std::string again = http_exchange(
      server.port(), "GET /metrics?debug=1 HTTP/1.1\r\nHost: localhost\r\n\r\n");
  require_contains(again, "HTTP/1.1 200 OK", "query strings are accepted");
}

void verify_renderer_exception_becomes_500() {
  grparse::MetricsHttpServer server(
      0, []() -> std::string { throw std::runtime_error("scheduler unavailable"); });
  const std::string response = http_exchange(
      server.port(), "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  require_contains(response, "HTTP/1.1 500 Internal Server Error", "renderer failure returns 500");
  require_contains(response, "scheduler unavailable", "failure reason is reported");
}

}  // namespace

int main() {
  try {
    verify_render_counters_and_gauges();
    verify_render_rotation_repair_and_office_cv_families();
    verify_render_histogram_is_cumulative();
    verify_http_listener();
    verify_renderer_exception_becomes_500();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "prometheus-metrics-test: {}", error.what());
    return EXIT_FAILURE;
  }
}
