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

#include "grparse/collector_coordinator.h"
#include "grparse/prometheus_metrics.h"
#include "support/check.h"

namespace {

using grparse_test::require;

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

void verify_render_ep_fallback_counter() {
  const std::string text = grparse::render_prometheus_metrics(
      grparse::PageScheduler::Metrics{}, grparse::OcrEnginePool::Stats{},
      grparse::PageScheduler::Options{}, grparse::RepairTotals{}, grparse::DataTotals{},
      grparse::OfficeCvTotals{}, 3, 2);
  require_contains(text, "# TYPE grparse_ort_ep_fallbacks_total counter\n", "fallback type line");
  require_contains(text, "grparse_ort_ep_fallbacks_total 3\n", "fallback counter value");
  require_contains(text, "# TYPE grparse_ort_ep_build_retries_total counter\n",
                   "build retries type line");
  require_contains(text, "grparse_ort_ep_build_retries_total 2\n", "build retries counter value");

  // The overloads that read the live process counter start at zero in a test
  // binary whose providers never refused anything.
  const std::string live = grparse::render_prometheus_metrics(
      grparse::PageScheduler::Metrics{}, grparse::OcrEnginePool::Stats{},
      grparse::PageScheduler::Options{});
  require_contains(live, "grparse_ort_ep_fallbacks_total 0\n", "live fallback counter starts at zero");
  require_contains(live, "grparse_ort_ep_build_retries_total 0\n",
                   "live build-retry counter starts at zero");
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
  repairs.furniture_tree_migrated = 22;
  repairs.orphans_repaired = 23;
  repairs.list_children_wrapped = 24;
  repairs.empty_groups_removed = 25;

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
      {"headings_split", 17},    {"headings_demoted", 18},        {"form_rows_split", 19},
      {"furniture_tree_migrated", 22}, {"orphans_repaired", 23},
      {"list_children_wrapped", 24},   {"empty_groups_removed", 25}};
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
  metrics.page_latency_ns = 1'500'000'000;

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
  require_contains(text, "grparse_page_latency_seconds_sum 1.5\n",
                   "a histogram carries its mandatory _sum series");
}

void verify_render_doctype_and_collector_families() {
  grparse::PipelineTelemetry telemetry;
  auto& pdf = telemetry.doctype("pdf");
  pdf.completed[static_cast<size_t>(grparse::DocumentCompletionStatus::kSuccess)] = 4;
  pdf.completed[static_cast<size_t>(grparse::DocumentCompletionStatus::kPartialSuccess)] = 1;
  pdf.completed[static_cast<size_t>(grparse::DocumentCompletionStatus::kFailure)] = 2;
  pdf.completed[static_cast<size_t>(grparse::DocumentCompletionStatus::kRejected)] = 3;
  pdf.duration.buckets[0] = 1;   // <= 0.01 s
  pdf.duration.buckets[3] = 2;   // <= 0.1 s
  pdf.duration.buckets[12] = 1;  // +Inf overflow (> 60 s)
  pdf.duration.sum_ns = 65'250'000'000ULL;

  auto& libreoffice = telemetry.collector("libreoffice");
  libreoffice.calls[static_cast<size_t>(grparse::CollectorCallStatus::kOk)] = 5;
  libreoffice.calls[static_cast<size_t>(grparse::CollectorCallStatus::kError)] = 1;
  libreoffice.calls[static_cast<size_t>(grparse::CollectorCallStatus::kTimeout)] = 2;
  libreoffice.calls[static_cast<size_t>(grparse::CollectorCallStatus::kUnavailable)] = 3;
  libreoffice.warnings = 9;
  libreoffice.duration.buckets[1] = 2;  // <= 0.025 s
  libreoffice.duration.buckets[6] = 3;  // <= 1 s
  libreoffice.duration.sum_ns = 2'500'000'000ULL;

  const std::string text = grparse::render_prometheus_metrics(
      grparse::PageScheduler::Metrics{}, grparse::OcrEnginePool::Stats{},
      grparse::PageScheduler::Options{}, grparse::RepairTotals{}, grparse::DataTotals{},
      grparse::OfficeCvTotals{}, 0, 0, telemetry);

  require_contains(text, "# HELP grparse_documents_completed_total ", "completed help line");
  require_contains(text, "# TYPE grparse_documents_completed_total counter\n",
                   "completed type line");
  require_contains(text,
                   "grparse_documents_completed_total{doctype=\"pdf\",status=\"success\"} 4\n",
                   "pdf success count");
  require_contains(
      text,
      "grparse_documents_completed_total{doctype=\"pdf\",status=\"partial_success\"} 1\n",
      "pdf partial_success count");
  require_contains(text,
                   "grparse_documents_completed_total{doctype=\"pdf\",status=\"failure\"} 2\n",
                   "pdf failure count");
  require_contains(text,
                   "grparse_documents_completed_total{doctype=\"pdf\",status=\"rejected\"} 3\n",
                   "pdf rejected count");
  for (std::string_view doctype : grparse::kMetricDoctypes) {
    require_contains(text,
                     "grparse_documents_completed_total{doctype=\"" + std::string(doctype) +
                         "\",status=\"success\"} ",
                     std::string("doctype series ") + std::string(doctype));
  }

  require_contains(text, "# HELP grparse_document_duration_seconds ", "doc duration help line");
  require_contains(text, "# TYPE grparse_document_duration_seconds histogram\n",
                   "doc duration type line");
  require_contains(
      text, "grparse_document_duration_seconds_bucket{doctype=\"pdf\",le=\"0.01\"} 1\n",
      "pdf duration 0.01 bucket");
  require_contains(
      text, "grparse_document_duration_seconds_bucket{doctype=\"pdf\",le=\"0.05\"} 1\n",
      "pdf duration 0.05 cumulative bucket");
  require_contains(
      text, "grparse_document_duration_seconds_bucket{doctype=\"pdf\",le=\"0.1\"} 3\n",
      "pdf duration 0.1 cumulative bucket");
  require_contains(
      text, "grparse_document_duration_seconds_bucket{doctype=\"pdf\",le=\"60\"} 3\n",
      "pdf duration 60 cumulative bucket");
  require_contains(
      text, "grparse_document_duration_seconds_bucket{doctype=\"pdf\",le=\"+Inf\"} 4\n",
      "pdf duration +Inf bucket");
  require_contains(text, "grparse_document_duration_seconds_sum{doctype=\"pdf\"} 65.25\n",
                   "pdf duration sum");
  require_contains(text, "grparse_document_duration_seconds_count{doctype=\"pdf\"} 4\n",
                   "pdf duration count");

  require_contains(text, "# HELP grparse_collector_calls_total ", "collector calls help line");
  require_contains(text, "# TYPE grparse_collector_calls_total counter\n",
                   "collector calls type line");
  require_contains(
      text, "grparse_collector_calls_total{collector=\"libreoffice\",status=\"ok\"} 5\n",
      "libreoffice ok calls");
  require_contains(
      text, "grparse_collector_calls_total{collector=\"libreoffice\",status=\"error\"} 1\n",
      "libreoffice error calls");
  require_contains(
      text, "grparse_collector_calls_total{collector=\"libreoffice\",status=\"timeout\"} 2\n",
      "libreoffice timeout calls");
  require_contains(
      text,
      "grparse_collector_calls_total{collector=\"libreoffice\",status=\"unavailable\"} 3\n",
      "libreoffice unavailable calls");
  for (std::string_view collector : grparse::kMetricCollectors) {
    require_contains(text,
                     "grparse_collector_calls_total{collector=\"" + std::string(collector) +
                         "\",status=\"ok\"} ",
                     std::string("collector series ") + std::string(collector));
  }

  require_contains(text, "# HELP grparse_collector_duration_seconds ",
                   "collector duration help line");
  require_contains(text, "# TYPE grparse_collector_duration_seconds histogram\n",
                   "collector duration type line");
  require_contains(
      text,
      "grparse_collector_duration_seconds_bucket{collector=\"libreoffice\",le=\"0.01\"} 0\n",
      "libreoffice duration 0.01 bucket");
  require_contains(
      text,
      "grparse_collector_duration_seconds_bucket{collector=\"libreoffice\",le=\"0.025\"} 2\n",
      "libreoffice duration 0.025 bucket");
  require_contains(
      text,
      "grparse_collector_duration_seconds_bucket{collector=\"libreoffice\",le=\"1\"} 5\n",
      "libreoffice duration 1s cumulative bucket");
  require_contains(
      text,
      "grparse_collector_duration_seconds_bucket{collector=\"libreoffice\",le=\"+Inf\"} 5\n",
      "libreoffice duration +Inf bucket");
  require_contains(
      text, "grparse_collector_duration_seconds_sum{collector=\"libreoffice\"} 2.5\n",
      "libreoffice duration sum");
  require_contains(
      text, "grparse_collector_duration_seconds_count{collector=\"libreoffice\"} 5\n",
      "libreoffice duration count");

  require_contains(text, "# HELP grparse_collector_warnings_total ",
                   "collector warnings help line");
  require_contains(text, "# TYPE grparse_collector_warnings_total counter\n",
                   "collector warnings type line");
  require_contains(text, "grparse_collector_warnings_total{collector=\"libreoffice\"} 9\n",
                   "libreoffice warnings count");
}

void verify_doctype_classification_and_collector_status_mapping() {
  using ai::pipestream::parse::v1::InputFormat;
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_PDF, "report.pdf",
                                    "application/pdf") == "pdf",
          "pdf classification");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_DOCX, "memo.docx", "") == "docx",
          "docx classification");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_XLSX, "sheet.ods", "") == "xlsx",
          "xlsx classification");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_PPTX, "deck.pptx", "") == "pptx",
          "pptx classification");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_HTML, "index.html", "") == "html",
          "html classification");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_MD, "README.md",
                                    "text/markdown") == "md",
          "markdown classification");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_MD, "notes.txt",
                                    "text/plain; charset=utf-8") == "txt",
          "plain text classified as txt even though input_format_for routes it to MD");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_CSV, "data.tsv", "text/csv") == "csv",
          "csv classification");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_IMAGE, "scan.png", "image/png") ==
              "image",
          "image classification");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_AUDIO, "clip.wav", "audio/wav") ==
              "audio",
          "audio classification");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_UNSPECIFIED, "book.epub",
                                    "application/epub+zip") == "epub",
          "epub classification from filename/mimetype");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_UNSPECIFIED, "mail.eml",
                                    "message/rfc822") == "email",
          "email classification");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_UNSPECIFIED, "feed.xml",
                                    "application/xml") == "xml",
          "xml classification");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_UNSPECIFIED, "crawl.warc.gz", "") ==
              "warc",
          "warc classification");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_UNSPECIFIED, "mainframe.ebcdic",
                                    "") == "ebcdic",
          "ebcdic classification");
  require(grparse::classify_doctype(InputFormat::INPUT_FORMAT_UNSPECIFIED, "unknown.bin",
                                    "application/octet-stream") == "other",
          "unknown input falls back to other");

  require(grparse::collector_call_status_for(true, grpc::StatusCode::OK) ==
              grparse::CollectorCallStatus::kOk,
          "OK status maps to ok");
  require(grparse::collector_call_status_for(false, grpc::StatusCode::DEADLINE_EXCEEDED) ==
              grparse::CollectorCallStatus::kTimeout,
          "DEADLINE_EXCEEDED maps to timeout");
  require(grparse::collector_call_status_for(false, grpc::StatusCode::UNAVAILABLE) ==
              grparse::CollectorCallStatus::kUnavailable,
          "UNAVAILABLE maps to unavailable");
  require(grparse::collector_call_status_for(false, grpc::StatusCode::UNIMPLEMENTED) ==
              grparse::CollectorCallStatus::kUnavailable,
          "unwired UNIMPLEMENTED collector maps to unavailable");
  require(grparse::collector_call_status_for(false, grpc::StatusCode::INVALID_ARGUMENT) ==
              grparse::CollectorCallStatus::kError,
          "other non-OK status maps to error");

  using ai::pipestream::parse::v1::Collector;
  require(grparse::metric_collector_name(Collector::COLLECTOR_PDF) == "pdf-inspector",
          "COLLECTOR_PDF maps to pdf-inspector");
  require(grparse::metric_collector_name(Collector::COLLECTOR_CONFLUENCE) == "confluence",
          "COLLECTOR_CONFLUENCE maps to confluence");
  require(grparse::metric_collector_name(Collector::COLLECTOR_GRPARSE_CV) == "grparse-cv",
          "COLLECTOR_GRPARSE_CV maps to grparse-cv");
}

void verify_live_pipeline_telemetry_and_run_collectors() {
  grparse::reset_pipeline_telemetry();

  grparse::record_document_completion("pdf", grparse::DocumentCompletionStatus::kSuccess,
                                      50'000'000ULL);
  grparse::record_document_completion(
      "pdf", grparse::DocumentCompletionStatus::kPartialSuccess, 200'000'000ULL);
  grparse::record_collector_call("enrich", grparse::CollectorCallStatus::kTimeout,
                                 5'000'000'000ULL, 2);
  grparse::record_collector_warnings("grparse-cv", 3);

  // Exercise run_collectors directly: a Confluence leg succeeds with 1 warning,
  // and an EPUB leg fails with UNAVAILABLE.
  std::vector<grparse::PlannedCollector> plan = {
      {.id = ai::pipestream::parse::v1::COLLECTOR_CONFLUENCE,
       .run =
           [] {
             grparse::CollectorOutcome ok;
             ok.success = true;
             ok.code = grpc::StatusCode::OK;
             ok.warnings = {"minor macro note"};
             return ok;
           }},
      {.id = ai::pipestream::parse::v1::COLLECTOR_EPUB,
       .run =
           [] {
             grparse::CollectorOutcome fail;
             fail.success = false;
             fail.code = grpc::StatusCode::UNAVAILABLE;
             fail.error = "connection refused";
             return fail;
           }},
  };
  const auto outcome =
      grparse::run_collectors(std::move(plan), ai::pipestream::document::v1::Document{});
  require(outcome.succeeded == 1, "one collector leg succeeded");
  require(outcome.failures.size() == 1, "one collector leg failed");

  grparse::MetricsHttpServer server(0, [] {
    return grparse::render_prometheus_metrics(
        grparse::PageScheduler::Metrics{}, grparse::OcrEnginePool::Stats{},
        grparse::PageScheduler::Options{});
  });
  const std::string scraped = http_exchange(
      server.port(), "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  require_contains(scraped, "HTTP/1.1 200 OK", "live metrics scrape succeeds");
  require_contains(
      scraped, "grparse_documents_completed_total{doctype=\"pdf\",status=\"success\"} 1\n",
      "scraped live pdf success count");
  require_contains(
      scraped,
      "grparse_documents_completed_total{doctype=\"pdf\",status=\"partial_success\"} 1\n",
      "scraped live pdf partial_success count");
  require_contains(
      scraped, "grparse_document_duration_seconds_bucket{doctype=\"pdf\",le=\"0.05\"} 1\n",
      "scraped live pdf 50ms bucket");
  require_contains(
      scraped, "grparse_document_duration_seconds_bucket{doctype=\"pdf\",le=\"0.25\"} 2\n",
      "scraped live pdf 250ms cumulative bucket");
  require_contains(scraped, "grparse_document_duration_seconds_count{doctype=\"pdf\"} 2\n",
                   "scraped live pdf duration count");
  require_contains(
      scraped, "grparse_collector_calls_total{collector=\"enrich\",status=\"timeout\"} 1\n",
      "scraped live enrich timeout call");
  require_contains(scraped, "grparse_collector_warnings_total{collector=\"enrich\"} 2\n",
                   "scraped live enrich warnings");
  require_contains(scraped, "grparse_collector_warnings_total{collector=\"grparse-cv\"} 3\n",
                   "scraped live grparse-cv warnings");
  require_contains(
      scraped, "grparse_collector_calls_total{collector=\"confluence\",status=\"ok\"} 1\n",
      "run_collectors recorded confluence ok call");
  require_contains(scraped, "grparse_collector_warnings_total{collector=\"confluence\"} 1\n",
                   "run_collectors recorded confluence warning");
  require_contains(
      scraped, "grparse_collector_calls_total{collector=\"epub\",status=\"unavailable\"} 1\n",
      "run_collectors recorded epub unavailable call");

  grparse::reset_pipeline_telemetry();
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
  return grparse_test::run_test_main("prometheus-metrics-test", {
      verify_render_counters_and_gauges,
      verify_render_rotation_repair_and_office_cv_families,
      verify_render_histogram_is_cumulative,
      verify_render_ep_fallback_counter,
      verify_render_doctype_and_collector_families,
      verify_doctype_classification_and_collector_status_mapping,
      verify_live_pipeline_telemetry_and_run_collectors,
      verify_http_listener,
      verify_renderer_exception_becomes_500,
  });
}
