// The render function is pure, so its output is asserted exactly; the HTTP
// listener is exercised over a real loopback socket (port 0) to prove the
// request routing, the content type, and the failure doors.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include "grparse/prometheus_metrics.h"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void require_contains(const std::string& haystack, const std::string& needle,
                      const std::string& message) {
  require(haystack.find(needle) != std::string::npos, message + " (missing: " + needle + ")");
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

  grparse::PageScheduler::Options options;
  options.render_workers = 4;
  options.inference_workers = 2;
  options.assembly_workers = 1;

  const std::string text = grparse::render_prometheus_metrics(metrics, ocr, options);
  require_contains(text, "grparse_documents_submitted_total 7\n", "submitted counter");
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
    verify_render_histogram_is_cumulative();
    verify_http_listener();
    verify_renderer_exception_becomes_500();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "prometheus-metrics-test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
