#include "grparse/prometheus_metrics.h"

#include "grparse/input_format.h"
#include "grparse_session_ep.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <print>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace grparse {
namespace {

constexpr const char* kContentType = "text/plain; version=0.0.4; charset=utf-8";

constexpr std::array<uint64_t, kDurationBoundsSeconds.size()> kDurationBoundsNs = {
    10'000'000ULL,     25'000'000ULL,     50'000'000ULL,     100'000'000ULL,
    250'000'000ULL,    500'000'000ULL,    1'000'000'000ULL,  2'500'000'000ULL,
    5'000'000'000ULL,  10'000'000'000ULL, 30'000'000'000ULL, 60'000'000'000ULL};

constexpr std::array<std::string_view, kDurationBoundsSeconds.size()> kDurationBoundLabels = {
    "0.01", "0.025", "0.05", "0.1", "0.25", "0.5", "1", "2.5", "5", "10", "30", "60"};

size_t duration_bucket_index(uint64_t duration_ns) {
  for (size_t i = 0; i < kDurationBoundsNs.size(); ++i) {
    if (duration_ns <= kDurationBoundsNs[i]) return i;
  }
  return kDurationBoundsNs.size();
}

struct AtomicDurationHistogram {
  std::array<std::atomic<uint64_t>, kDurationBoundsSeconds.size() + 1> buckets{};
  std::atomic<uint64_t> sum_ns{0};

  void observe(uint64_t duration_ns) {
    buckets[duration_bucket_index(duration_ns)].fetch_add(1, std::memory_order_relaxed);
    sum_ns.fetch_add(duration_ns, std::memory_order_relaxed);
  }

  DurationHistogram snapshot() const {
    DurationHistogram out;
    for (size_t i = 0; i < buckets.size(); ++i) {
      out.buckets[i] = buckets[i].load(std::memory_order_relaxed);
    }
    out.sum_ns = sum_ns.load(std::memory_order_relaxed);
    return out;
  }

  void reset() {
    for (auto& bucket : buckets) {
      bucket.store(0, std::memory_order_relaxed);
    }
    sum_ns.store(0, std::memory_order_relaxed);
  }
};

struct AtomicDoctypeMetrics {
  std::array<std::atomic<uint64_t>, kDocumentCompletionStatuses.size()> completed{};
  AtomicDurationHistogram duration{};

  DoctypeMetrics snapshot() const {
    DoctypeMetrics out;
    for (size_t i = 0; i < completed.size(); ++i) {
      out.completed[i] = completed[i].load(std::memory_order_relaxed);
    }
    out.duration = duration.snapshot();
    return out;
  }

  void reset() {
    for (auto& count : completed) {
      count.store(0, std::memory_order_relaxed);
    }
    duration.reset();
  }
};

struct AtomicCollectorMetrics {
  std::array<std::atomic<uint64_t>, kCollectorCallStatuses.size()> calls{};
  AtomicDurationHistogram duration{};
  std::atomic<uint64_t> warnings{0};

  CollectorMetrics snapshot() const {
    CollectorMetrics out;
    for (size_t i = 0; i < calls.size(); ++i) {
      out.calls[i] = calls[i].load(std::memory_order_relaxed);
    }
    out.duration = duration.snapshot();
    out.warnings = warnings.load(std::memory_order_relaxed);
    return out;
  }

  void reset() {
    for (auto& count : calls) {
      count.store(0, std::memory_order_relaxed);
    }
    duration.reset();
    warnings.store(0, std::memory_order_relaxed);
  }
};

std::array<AtomicDoctypeMetrics, kMetricDoctypes.size()> g_doctypes{};
std::array<AtomicCollectorMetrics, kMetricCollectors.size()> g_collectors{};

size_t doctype_index(std::string_view name) {
  for (size_t i = 0; i < kMetricDoctypes.size(); ++i) {
    if (kMetricDoctypes[i] == name) return i;
  }
  return kMetricDoctypes.size() - 1;  // "other"
}

std::optional<size_t> collector_index(std::string_view name) {
  if (name == "pdf") name = "pdf-inspector";
  if (name == "confluence-storage") name = "confluence";
  for (size_t i = 0; i < kMetricCollectors.size(); ++i) {
    if (kMetricCollectors[i] == name) return i;
  }
  return std::nullopt;
}

std::string lower_ascii(std::string_view input) {
  std::string out(input);
  std::ranges::transform(out, out.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string_view trimmed_mime(std::string_view mime_type) {
  const size_t semi = mime_type.find(';');
  if (semi != std::string_view::npos) mime_type = mime_type.substr(0, semi);
  while (!mime_type.empty() && std::isspace(static_cast<unsigned char>(mime_type.front()))) {
    mime_type.remove_prefix(1);
  }
  while (!mime_type.empty() && std::isspace(static_cast<unsigned char>(mime_type.back()))) {
    mime_type.remove_suffix(1);
  }
  return mime_type;
}

std::optional<std::string_view> doctype_from_input_format(
    ai::pipestream::parse::v1::InputFormat format) {
  using ai::pipestream::parse::v1::InputFormat;
  switch (format) {
    case InputFormat::INPUT_FORMAT_PDF:
      return "pdf";
    case InputFormat::INPUT_FORMAT_DOCX:
    case InputFormat::INPUT_FORMAT_DOC:
    case InputFormat::INPUT_FORMAT_ODT:
    case InputFormat::INPUT_FORMAT_RTF:
    case InputFormat::INPUT_FORMAT_IWORK_PAGES:
      return "docx";
    case InputFormat::INPUT_FORMAT_XLSX:
    case InputFormat::INPUT_FORMAT_XLS:
    case InputFormat::INPUT_FORMAT_ODS:
      return "xlsx";
    case InputFormat::INPUT_FORMAT_PPTX:
    case InputFormat::INPUT_FORMAT_PPT:
    case InputFormat::INPUT_FORMAT_ODP:
    case InputFormat::INPUT_FORMAT_IWORK_KEYNOTE:
      return "pptx";
    case InputFormat::INPUT_FORMAT_HTML:
    case InputFormat::INPUT_FORMAT_MHTML:
      return "html";
    case InputFormat::INPUT_FORMAT_MD:
    case InputFormat::INPUT_FORMAT_ASCIIDOC:
    case InputFormat::INPUT_FORMAT_LATEX:
    case InputFormat::INPUT_FORMAT_VTT:
    case InputFormat::INPUT_FORMAT_BOXNOTE:
      return "md";
    case InputFormat::INPUT_FORMAT_EPUB:
      return "epub";
    case InputFormat::INPUT_FORMAT_EMAIL:
      return "email";
    case InputFormat::INPUT_FORMAT_XML_JATS:
    case InputFormat::INPUT_FORMAT_XML_USPTO:
    case InputFormat::INPUT_FORMAT_XML_XBRL:
    case InputFormat::INPUT_FORMAT_XML_DOCLANG:
      return "xml";
    case InputFormat::INPUT_FORMAT_IMAGE:
      return "image";
    case InputFormat::INPUT_FORMAT_AUDIO:
    case InputFormat::INPUT_FORMAT_VIDEO:
      return "audio";
    case InputFormat::INPUT_FORMAT_CSV:
      return "csv";
    default:
      return std::nullopt;
  }
}

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

void DurationHistogram::observe(uint64_t duration_ns) {
  ++buckets[duration_bucket_index(duration_ns)];
  sum_ns += duration_ns;
}

uint64_t DurationHistogram::count() const {
  uint64_t total = 0;
  for (const uint64_t bucket_count : buckets) {
    total += bucket_count;
  }
  return total;
}

DoctypeMetrics& PipelineTelemetry::doctype(std::string_view name) {
  return doctypes[doctype_index(name)];
}

const DoctypeMetrics& PipelineTelemetry::doctype(std::string_view name) const {
  return doctypes[doctype_index(name)];
}

CollectorMetrics& PipelineTelemetry::collector(std::string_view name) {
  const auto index = collector_index(name);
  if (!index.has_value()) {
    throw std::out_of_range("unknown collector metric label: " + std::string(name));
  }
  return collectors[*index];
}

const CollectorMetrics& PipelineTelemetry::collector(std::string_view name) const {
  const auto index = collector_index(name);
  if (!index.has_value()) {
    throw std::out_of_range("unknown collector metric label: " + std::string(name));
  }
  return collectors[*index];
}

PipelineTelemetry pipeline_telemetry() {
  PipelineTelemetry out;
  for (size_t i = 0; i < kMetricDoctypes.size(); ++i) {
    out.doctypes[i] = g_doctypes[i].snapshot();
  }
  for (size_t i = 0; i < kMetricCollectors.size(); ++i) {
    out.collectors[i] = g_collectors[i].snapshot();
  }
  return out;
}

void reset_pipeline_telemetry() {
  for (auto& doctype : g_doctypes) {
    doctype.reset();
  }
  for (auto& collector : g_collectors) {
    collector.reset();
  }
}

void record_document_completion(std::string_view doctype, DocumentCompletionStatus status,
                                uint64_t duration_ns) {
  auto& target = g_doctypes[doctype_index(doctype)];
  target.completed[static_cast<size_t>(status)].fetch_add(1, std::memory_order_relaxed);
  target.duration.observe(duration_ns);
}

void record_collector_call(std::string_view collector, CollectorCallStatus status,
                           uint64_t duration_ns, uint64_t warnings) {
  const auto index = collector_index(collector);
  if (!index.has_value()) return;
  auto& target = g_collectors[*index];
  target.calls[static_cast<size_t>(status)].fetch_add(1, std::memory_order_relaxed);
  target.duration.observe(duration_ns);
  if (warnings > 0) {
    target.warnings.fetch_add(warnings, std::memory_order_relaxed);
  }
}

void record_collector_call(ai::pipestream::parse::v1::Collector collector,
                           CollectorCallStatus status, uint64_t duration_ns, uint64_t warnings) {
  record_collector_call(metric_collector_name(collector), status, duration_ns, warnings);
}

void record_collector_warnings(std::string_view collector, uint64_t count) {
  if (count == 0) return;
  const auto index = collector_index(collector);
  if (!index.has_value()) return;
  g_collectors[*index].warnings.fetch_add(count, std::memory_order_relaxed);
}

void record_collector_warnings(ai::pipestream::parse::v1::Collector collector, uint64_t count) {
  record_collector_warnings(metric_collector_name(collector), count);
}

CollectorCallStatus collector_call_status_for(bool success, grpc::StatusCode code) {
  if (success) return CollectorCallStatus::kOk;
  switch (code) {
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return CollectorCallStatus::kTimeout;
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::FAILED_PRECONDITION:
    case grpc::StatusCode::UNIMPLEMENTED:
      return CollectorCallStatus::kUnavailable;
    default:
      return CollectorCallStatus::kError;
  }
}

std::string_view metric_collector_name(ai::pipestream::parse::v1::Collector collector) {
  using ai::pipestream::parse::v1::Collector;
  switch (collector) {
    case Collector::COLLECTOR_GRPARSE_CV:
      return "grparse-cv";
    case Collector::COLLECTOR_LIBREOFFICE:
      return "libreoffice";
    case Collector::COLLECTOR_ASR:
      return "asr";
    case Collector::COLLECTOR_EMAIL:
      return "email";
    case Collector::COLLECTOR_XML:
      return "xml";
    case Collector::COLLECTOR_EBCDIC:
      return "ebcdic";
    case Collector::COLLECTOR_EPUB:
      return "epub";
    case Collector::COLLECTOR_MARKUP:
      return "markup";
    case Collector::COLLECTOR_LOL_HTML:
      return "lol-html";
    case Collector::COLLECTOR_FASTWARC:
      return "fastwarc";
    case Collector::COLLECTOR_PDF:
      return "pdf-inspector";
    case Collector::COLLECTOR_CONFLUENCE:
      return "confluence";
    case Collector::COLLECTOR_CALAMINE:
      return "calamine";
    case Collector::COLLECTOR_UNSPECIFIED:
    default:
      return "";
  }
}

std::string_view classify_doctype(ai::pipestream::parse::v1::InputFormat format,
                                  std::string_view filename, std::string_view mime_type,
                                  std::string_view content) {
  const std::string lower_name = lower_ascii(filename);
  const std::string lower_mime = lower_ascii(trimmed_mime(mime_type));
  const std::string ext = std::filesystem::path(lower_name).extension().string();

  const bool is_txt_hint = (ext == ".txt" || lower_mime == "text/plain");
  if (format == ai::pipestream::parse::v1::INPUT_FORMAT_MD && is_txt_hint) {
    return "txt";
  }
  if (const auto mapped = doctype_from_input_format(format); mapped.has_value()) {
    return *mapped;
  }
  if (format != ai::pipestream::parse::v1::INPUT_FORMAT_UNSPECIFIED) {
    return "other";
  }

  if (lower_name.ends_with(".warc") || lower_name.ends_with(".warc.gz") ||
      lower_mime == "application/warc" || lower_mime == "application/x-warc" ||
      content.starts_with("WARC/")) {
    return "warc";
  }
  if (ext == ".ebc" || ext == ".ebcdic" || lower_mime == "application/ebcdic" ||
      lower_mime == "application/x-ebcdic") {
    return "ebcdic";
  }
  if (is_txt_hint) {
    return "txt";
  }
  if (const auto detected = input_format_for(lower_mime, std::filesystem::path(lower_name));
      detected.has_value()) {
    if (const auto mapped = doctype_from_input_format(*detected); mapped.has_value()) {
      return *mapped;
    }
    return "other";
  }
  if (ext == ".xml" || ext == ".xsd" || ext == ".xsl" || ext == ".xslt" ||
      lower_mime.ends_with("+xml")) {
    return "xml";
  }
  if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tif" || ext == ".tiff" ||
      ext == ".bmp" || ext == ".webp" || ext == ".gif") {
    return "image";
  }
  if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac" || ext == ".m4a" ||
      ext == ".aac" || ext == ".opus" || ext == ".mp4" || ext == ".webm") {
    return "audio";
  }
  if (content.starts_with("%PDF-")) {
    return "pdf";
  }
  return "other";
}

std::string render_prometheus_metrics(const PageScheduler::Metrics& metrics,
                                      const OcrEnginePool::Stats& ocr_pool,
                                      const PageScheduler::Options& options,
                                      const RepairTotals& repairs) {
  return render_prometheus_metrics(metrics, ocr_pool, options, repairs, data_totals());
}

std::string render_prometheus_metrics(const PageScheduler::Metrics& metrics,
                                      const OcrEnginePool::Stats& ocr_pool,
                                      const PageScheduler::Options& options,
                                      const RepairTotals& repairs, const DataTotals& data) {
  return render_prometheus_metrics(metrics, ocr_pool, options, repairs, data, office_cv_totals());
}

std::string render_prometheus_metrics(const PageScheduler::Metrics& metrics,
                                      const OcrEnginePool::Stats& ocr_pool,
                                      const PageScheduler::Options& options,
                                      const RepairTotals& repairs, const DataTotals& data,
                                      const OfficeCvTotals& office_cv) {
  return render_prometheus_metrics(metrics, ocr_pool, options, repairs, data, office_cv,
                                   ep_fallback_count(), ep_build_retry_count());
}

std::string render_prometheus_metrics(const PageScheduler::Metrics& metrics,
                                      const OcrEnginePool::Stats& ocr_pool,
                                      const PageScheduler::Options& options,
                                      const RepairTotals& repairs, const DataTotals& data,
                                      const OfficeCvTotals& office_cv, uint64_t ep_fallbacks,
                                      uint64_t ep_build_retries) {
  return render_prometheus_metrics(metrics, ocr_pool, options, repairs, data, office_cv,
                                   ep_fallbacks, ep_build_retries, pipeline_telemetry());
}

std::string render_prometheus_metrics(const PageScheduler::Metrics& metrics,
                                      const OcrEnginePool::Stats& ocr_pool,
                                      const PageScheduler::Options& options,
                                      const RepairTotals& repairs, const DataTotals& data,
                                      const OfficeCvTotals& office_cv, uint64_t ep_fallbacks,
                                      uint64_t ep_build_retries,
                                      const PipelineTelemetry& telemetry) {
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

  counter(out, "grparse_pages_rerecognized_total",
          "Pages read more than once to recover their orientation.", metrics.pages_rerecognized);
  counter(out, "grparse_rerecognition_passes_total",
          "Extra recognition passes spent on orientation recovery.",
          metrics.rerecognition_passes);
  out << "# HELP grparse_page_rotations_total Pages whose kept read was the raster turned "
         "clockwise by the labelled degrees.\n"
         "# TYPE grparse_page_rotations_total counter\n";
  for (size_t turn = 0; turn < PageScheduler::kRotationDegrees.size(); ++turn) {
    out << "grparse_page_rotations_total{degrees=\"" << PageScheduler::kRotationDegrees[turn]
        << "\"} " << metrics.rotations_applied[turn] << '\n';
  }

  out << "# HELP grparse_repair_changes_total Changes the post-merge repair pass made to "
         "finished documents, by kind.\n"
         "# TYPE grparse_repair_changes_total counter\n"
      << "grparse_repair_changes_total{kind=\"furniture_demoted\"} " << repairs.furniture_demoted
      << '\n'
      << "grparse_repair_changes_total{kind=\"hyphens_rejoined\"} " << repairs.hyphens_rejoined
      << '\n'
      << "grparse_repair_changes_total{kind=\"paragraphs_merged\"} " << repairs.paragraphs_merged
      << '\n'
      << "grparse_repair_changes_total{kind=\"titles_merged\"} " << repairs.titles_merged << '\n'
      << "grparse_repair_changes_total{kind=\"heading_levels_assigned\"} "
      << repairs.heading_levels_assigned << '\n'
      << "grparse_repair_changes_total{kind=\"body_items_reordered\"} "
      << repairs.body_items_reordered << '\n'
      << "grparse_repair_changes_total{kind=\"headings_split\"} " << repairs.headings_split
      << '\n'
      << "grparse_repair_changes_total{kind=\"headings_demoted\"} " << repairs.headings_demoted
      << '\n'
      << "grparse_repair_changes_total{kind=\"form_rows_split\"} " << repairs.form_rows_split
      << '\n'
      << "grparse_repair_changes_total{kind=\"furniture_tree_migrated\"} "
      << repairs.furniture_tree_migrated << '\n'
      << "grparse_repair_changes_total{kind=\"orphans_repaired\"} " << repairs.orphans_repaired
      << '\n'
      << "grparse_repair_changes_total{kind=\"list_children_wrapped\"} "
      << repairs.list_children_wrapped << '\n'
      << "grparse_repair_changes_total{kind=\"empty_groups_removed\"} "
      << repairs.empty_groups_removed << '\n';

  out << "# HELP grparse_office_cv_total Figures the office CV enrichment added to mapped "
         "office documents, by kind.\n"
         "# TYPE grparse_office_cv_total counter\n"
      << "grparse_office_cv_total{kind=\"pictures_added\"} " << office_cv.pictures_added << '\n'
      << "grparse_office_cv_total{kind=\"pictures_anchored\"} " << office_cv.pictures_anchored
      << '\n';

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
      << data.cv_enrichment_skipped << '\n'
      << "grparse_data_changes_total{kind=\"charts_derendered\"} " << data.charts_derendered
      << '\n'
      << "grparse_data_changes_total{kind=\"chart_derender_skipped\"} "
      << data.chart_derender_skipped << '\n';

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
  counter(out, "grparse_ort_ep_fallbacks_total",
          "Sessions rebuilt on CPU after their selected execution provider refused the graph "
          "or the device failed.",
          ep_fallbacks);
  counter(out, "grparse_ort_ep_build_retries_total",
          "Session builds retried after a transient OpenVINO execution-provider failure "
          "before either succeeding or failing.",
          ep_build_retries);
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
  out << "grparse_page_latency_seconds_sum " << seconds(metrics.page_latency_ns) << '\n'
      << "grparse_page_latency_seconds_count " << cumulative << '\n';

  out << "# HELP grparse_documents_completed_total Completed document conversions by doctype "
         "and status.\n"
         "# TYPE grparse_documents_completed_total counter\n";
  for (size_t i = 0; i < kMetricDoctypes.size(); ++i) {
    const std::string_view doctype = kMetricDoctypes[i];
    const DoctypeMetrics& dt = telemetry.doctypes[i];
    for (size_t s = 0; s < kDocumentCompletionStatuses.size(); ++s) {
      out << "grparse_documents_completed_total{doctype=\"" << doctype << "\",status=\""
          << kDocumentCompletionStatuses[s] << "\"} " << dt.completed[s] << '\n';
    }
  }

  out << "# HELP grparse_document_duration_seconds End-to-end document conversion latency in "
         "seconds, by doctype.\n"
         "# TYPE grparse_document_duration_seconds histogram\n";
  for (size_t i = 0; i < kMetricDoctypes.size(); ++i) {
    const std::string_view doctype = kMetricDoctypes[i];
    const DurationHistogram& hist = telemetry.doctypes[i].duration;
    uint64_t doc_cumulative = 0;
    for (size_t bucket = 0; bucket < hist.buckets.size(); ++bucket) {
      doc_cumulative += hist.buckets[bucket];
      out << "grparse_document_duration_seconds_bucket{doctype=\"" << doctype << "\",le=\"";
      if (bucket < kDurationBoundLabels.size()) {
        out << kDurationBoundLabels[bucket];
      } else {
        out << "+Inf";
      }
      out << "\"} " << doc_cumulative << '\n';
    }
    out << "grparse_document_duration_seconds_sum{doctype=\"" << doctype << "\"} "
        << seconds(hist.sum_ns) << '\n'
        << "grparse_document_duration_seconds_count{doctype=\"" << doctype << "\"} "
        << doc_cumulative << '\n';
  }

  out << "# HELP grparse_collector_calls_total Collector invocations by collector and status.\n"
         "# TYPE grparse_collector_calls_total counter\n";
  for (size_t i = 0; i < kMetricCollectors.size(); ++i) {
    const std::string_view collector = kMetricCollectors[i];
    const CollectorMetrics& cm = telemetry.collectors[i];
    for (size_t s = 0; s < kCollectorCallStatuses.size(); ++s) {
      out << "grparse_collector_calls_total{collector=\"" << collector << "\",status=\""
          << kCollectorCallStatuses[s] << "\"} " << cm.calls[s] << '\n';
    }
  }

  out << "# HELP grparse_collector_duration_seconds Collector call latency in seconds, by "
         "collector.\n"
         "# TYPE grparse_collector_duration_seconds histogram\n";
  for (size_t i = 0; i < kMetricCollectors.size(); ++i) {
    const std::string_view collector = kMetricCollectors[i];
    const DurationHistogram& hist = telemetry.collectors[i].duration;
    uint64_t col_cumulative = 0;
    for (size_t bucket = 0; bucket < hist.buckets.size(); ++bucket) {
      col_cumulative += hist.buckets[bucket];
      out << "grparse_collector_duration_seconds_bucket{collector=\"" << collector << "\",le=\"";
      if (bucket < kDurationBoundLabels.size()) {
        out << kDurationBoundLabels[bucket];
      } else {
        out << "+Inf";
      }
      out << "\"} " << col_cumulative << '\n';
    }
    out << "grparse_collector_duration_seconds_sum{collector=\"" << collector << "\"} "
        << seconds(hist.sum_ns) << '\n'
        << "grparse_collector_duration_seconds_count{collector=\"" << collector << "\"} "
        << col_cumulative << '\n';
  }

  out << "# HELP grparse_collector_warnings_total Warnings emitted by collector legs, by "
         "collector.\n"
         "# TYPE grparse_collector_warnings_total counter\n";
  for (size_t i = 0; i < kMetricCollectors.size(); ++i) {
    out << "grparse_collector_warnings_total{collector=\"" << kMetricCollectors[i] << "\"} "
        << telemetry.collectors[i].warnings << '\n';
  }

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
  constexpr std::chrono::milliseconds kMaximumBackoff{1000};
  std::chrono::milliseconds backoff{0};
  while (!stopping_.load()) {
    const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      if (stopping_.load()) return;
      if (errno == EINTR || errno == ECONNABORTED) continue;
      // EMFILE, ENFILE, ENOBUFS and the like are pressure that passes, not a
      // dead listener: say so once per run of failures and retry, backing off,
      // rather than letting /metrics go dark for the life of the process.
      if (backoff.count() == 0) {
        std::println(stderr, "Metrics exporter: accept failed ({}); retrying",
                     std::strerror(errno));
      }
      backoff = std::min(kMaximumBackoff, std::max(std::chrono::milliseconds(10), backoff * 2));
      std::this_thread::sleep_for(backoff);
      continue;
    }
    backoff = std::chrono::milliseconds(0);
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
