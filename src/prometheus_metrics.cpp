#include "grparse/prometheus_metrics.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace grparse {
namespace {

constexpr const char* kContentType = "text/plain; version=0.0.4; charset=utf-8";

void counter(std::ostringstream& out, const char* name, const char* help, uint64_t value) {
  out << "# HELP " << name << ' ' << help << "\n# TYPE " << name << " counter\n"
      << name << ' ' << value << '\n';
}

void gauge(std::ostringstream& out, const char* name, const char* help, uint64_t value) {
  out << "# HELP " << name << ' ' << help << "\n# TYPE " << name << " gauge\n"
      << name << ' ' << value << '\n';
}

double seconds(uint64_t nanoseconds) { return static_cast<double>(nanoseconds) * 1e-9; }

std::string http_response(const char* status_line, const std::string& content_type,
                          const std::string& body) {
  std::ostringstream response;
  response << "HTTP/1.1 " << status_line << "\r\nContent-Type: " << content_type
           << "\r\nContent-Length: " << body.size() << "\r\nConnection: close\r\n\r\n"
           << body;
  return response.str();
}

// Resident set size from /proc/self/statm and CPU time from getrusage:
// both are one syscall or one small read, cheap enough for every scrape.
struct ProcessUsage {
  uint64_t resident_bytes = 0;
  double cpu_seconds = 0.0;
};

ProcessUsage read_process_usage() {
  ProcessUsage usage;
  if (std::ifstream statm("/proc/self/statm"); statm) {
    uint64_t total_pages = 0;
    uint64_t resident_pages = 0;
    if (statm >> total_pages >> resident_pages) {
      const long page_size = ::sysconf(_SC_PAGESIZE);
      usage.resident_bytes = resident_pages * static_cast<uint64_t>(page_size > 0 ? page_size : 4096);
    }
  }
  struct rusage self {};
  if (::getrusage(RUSAGE_SELF, &self) == 0) {
    usage.cpu_seconds = static_cast<double>(self.ru_utime.tv_sec) +
                        static_cast<double>(self.ru_utime.tv_usec) / 1e6 +
                        static_cast<double>(self.ru_stime.tv_sec) +
                        static_cast<double>(self.ru_stime.tv_usec) / 1e6;
  }
  return usage;
}

void write_all(int fd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t wrote = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (wrote <= 0) return;  // Peer went away; a scrape is never worth retrying.
    sent += static_cast<size_t>(wrote);
  }
}

}  // namespace

std::string render_prometheus_metrics(const PageScheduler::Metrics& metrics,
                                      const OcrEnginePool::Stats& ocr_pool,
                                      const PageScheduler::Options& options,
                                      const RepairTotals& repairs) {
  return render_prometheus_metrics(metrics, ocr_pool, options, repairs, data_totals());
}

std::string render_prometheus_metrics(const PageScheduler::Metrics& metrics,
                                      const OcrEnginePool::Stats& ocr_pool,
                                      const PageScheduler::Options& options,
                                      const RepairTotals& repairs,
                                      const DataTotals& data) {
  std::ostringstream out;
  out.precision(15);

  counter(out, "grparse_documents_submitted_total", "Documents accepted for parsing.",
          metrics.documents_submitted);
  counter(out, "grparse_documents_rejected_total",
          "Documents rejected at admission (queue full or invalid).", metrics.documents_rejected);
  gauge(out, "grparse_documents_queued", "Documents admitted and waiting to start.",
        metrics.documents_queued);

  counter(out, "grparse_pages_digital_total", "Pages whose text came from the digital layer.",
          metrics.pages_read_digitally);
  counter(out, "grparse_pages_rendered_total", "Pages rasterized for inference.",
          metrics.pages_rendered);
  counter(out, "grparse_pages_ocr_total", "Pages that went through OCR.", metrics.pages_recognized);
  counter(out, "grparse_pages_layout_total", "Pages that went through layout region detection.",
          metrics.pages_layout_labelled);
  counter(out, "grparse_tables_structured_total",
          "Table regions that went through structure recognition.", metrics.tables_structured);
  counter(out, "grparse_figures_classified_total",
          "Figure regions that went through classification.", metrics.figures_classified);
  counter(out, "grparse_barcodes_decoded_total", "Barcode payloads decoded from figure crops.",
          metrics.barcodes_decoded);
  counter(out, "grparse_pages_cancelled_total", "Pages abandoned by cancelled documents.",
          metrics.pages_cancelled);

  out << "# HELP grparse_repairs_total Changes the post-merge repair pass made to finished "
         "documents, by kind.\n"
         "# TYPE grparse_repairs_total counter\n"
      << "grparse_repairs_total{kind=\"furniture_demoted\"} " << repairs.furniture_demoted << '\n'
      << "grparse_repairs_total{kind=\"hyphens_rejoined\"} " << repairs.hyphens_rejoined << '\n'
      << "grparse_repairs_total{kind=\"paragraphs_merged\"} " << repairs.paragraphs_merged << '\n';

  out << "# HELP grparse_data_changes_total Data-plane changes made to finished documents, "
         "by kind.\n"
         "# TYPE grparse_data_changes_total counter\n"
      << "grparse_data_changes_total{kind=\"charts_bound\"} " << data.charts_bound << '\n'
      << "grparse_data_changes_total{kind=\"chart_captions\"} " << data.chart_captions << '\n'
      << "grparse_data_changes_total{kind=\"sheet_header_rows\"} " << data.sheet_header_rows
      << '\n'
      << "grparse_data_changes_total{kind=\"mimetypes_sniffed\"} " << data.mimetypes_sniffed
      << '\n'
      << "grparse_data_changes_total{kind=\"cv_enrichment_skipped\"} "
      << data.cv_enrichment_skipped << '\n';

  // The process gauges the standard client libraries export, so a scrape
  // can chart memory and CPU beside the pipeline counters.
  const ProcessUsage usage = read_process_usage();
  out << "# HELP process_resident_memory_bytes Resident memory size in bytes.\n"
         "# TYPE process_resident_memory_bytes gauge\n"
      << "process_resident_memory_bytes " << usage.resident_bytes << '\n'
      << "# HELP process_cpu_seconds_total Total user and system CPU time spent in seconds.\n"
         "# TYPE process_cpu_seconds_total counter\n"
      << "process_cpu_seconds_total " << usage.cpu_seconds << '\n';

  out << "# HELP grparse_pages_waiting Pages queued ahead of a pipeline stage.\n"
         "# TYPE grparse_pages_waiting gauge\n"
      << "grparse_pages_waiting{stage=\"render\"} " << metrics.pages_waiting_for_render << '\n'
      << "grparse_pages_waiting{stage=\"inference\"} " << metrics.pages_waiting_for_inference
      << '\n'
      << "grparse_pages_waiting{stage=\"assembly\"} " << metrics.pages_waiting_for_assembly
      << '\n';

  out << "# HELP grparse_stage_busy_seconds_total Seconds a stage's workers spent doing page "
         "work (not blocked on queues); divide the rate by grparse_stage_workers for a busy "
         "fraction.\n"
         "# TYPE grparse_stage_busy_seconds_total counter\n"
      << "grparse_stage_busy_seconds_total{stage=\"render\"} " << seconds(metrics.render_busy_ns)
      << '\n'
      << "grparse_stage_busy_seconds_total{stage=\"inference\"} "
      << seconds(metrics.inference_busy_ns) << '\n'
      << "grparse_stage_busy_seconds_total{stage=\"assembly\"} "
      << seconds(metrics.assembly_busy_ns) << '\n';

  out << "# HELP grparse_stage_workers Configured worker count per pipeline stage.\n"
         "# TYPE grparse_stage_workers gauge\n"
      << "grparse_stage_workers{stage=\"render\"} " << options.render_workers << '\n'
      << "grparse_stage_workers{stage=\"inference\"} " << options.inference_workers << '\n'
      << "grparse_stage_workers{stage=\"assembly\"} " << options.assembly_workers << '\n';

  counter(out, "grparse_ocr_pool_acquires_total", "Warm OCR session leases handed out.",
          ocr_pool.acquires);
  counter(out, "grparse_ocr_pool_discards_total",
          "OCR sessions rebuilt after a device error.", ocr_pool.discards);
  out << "# HELP grparse_ocr_pool_wait_seconds_total Seconds inference workers waited for a "
         "warm OCR session.\n"
         "# TYPE grparse_ocr_pool_wait_seconds_total counter\n"
      << "grparse_ocr_pool_wait_seconds_total " << seconds(ocr_pool.wait_ns) << '\n';

  out << "# HELP grparse_page_latency_seconds Completed pages by schedule-to-delivered "
         "latency.\n"
         "# TYPE grparse_page_latency_seconds histogram\n";
  uint64_t cumulative = 0;
  for (size_t bucket = 0; bucket < metrics.page_latency.size(); ++bucket) {
    cumulative += metrics.page_latency[bucket];
    out << "grparse_page_latency_seconds_bucket{le=\"";
    if (bucket < PageScheduler::kPageLatencyBoundsMs.size()) {
      out << static_cast<double>(PageScheduler::kPageLatencyBoundsMs[bucket]) / 1000.0;
    } else {
      out << "+Inf";
    }
    out << "\"} " << cumulative << '\n';
  }
  out << "grparse_page_latency_seconds_count " << cumulative << '\n';

  return out.str();
}

MetricsHttpServer::MetricsHttpServer(uint16_t port, Renderer render) : render_(std::move(render)) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    throw std::runtime_error(std::string("Metrics exporter: socket() failed: ") +
                             std::strerror(errno));
  }
  const int enable = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(listen_fd_, 8) != 0) {
    const std::string reason = std::strerror(errno);
    ::close(listen_fd_);
    throw std::runtime_error("Metrics exporter: could not listen on port " +
                             std::to_string(port) + ": " + reason);
  }
  socklen_t address_length = sizeof(address);
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &address_length);
  port_ = ntohs(address.sin_port);
  thread_ = std::thread([this] { serve(); });
}

MetricsHttpServer::~MetricsHttpServer() {
  stopping_.store(true);
  // shutdown() wakes the blocked accept(); the fd closes only after the
  // thread joins, so accept can never race a reused descriptor.
  ::shutdown(listen_fd_, SHUT_RDWR);
  if (thread_.joinable()) thread_.join();
  ::close(listen_fd_);
}

void MetricsHttpServer::serve() {
  while (!stopping_.load()) {
    const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      if (stopping_.load()) return;
      if (errno == EINTR || errno == ECONNABORTED) continue;
      return;
    }
    // Bounded I/O so one stuck scraper cannot wedge the single serving thread.
    timeval timeout{};
    timeout.tv_sec = 5;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::string request;
    char buffer[1024];
    while (request.size() < 8192 && !request.contains("\r\n\r\n")) {
      const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
      if (received <= 0) break;
      request.append(buffer, static_cast<size_t>(received));
    }

    const size_t line_end = request.find("\r\n");
    const std::string request_line = request.substr(0, line_end);
    if (!request_line.starts_with("GET ")) {
      write_all(client, http_response("405 Method Not Allowed", "text/plain",
                                      "only GET is supported\n"));
    } else if (request_line.starts_with("GET /metrics ") ||
               request_line.starts_with("GET /metrics?")) {
      try {
        write_all(client, http_response("200 OK", kContentType, render_()));
      } catch (const std::exception& error) {
        write_all(client, http_response("500 Internal Server Error", "text/plain",
                                        std::string("metrics rendering failed: ") + error.what() +
                                            "\n"));
      }
    } else {
      write_all(client, http_response("404 Not Found", "text/plain", "see /metrics\n"));
    }
    ::close(client);
  }
}

}  // namespace grparse
