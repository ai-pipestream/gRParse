// grparse-bench: the performance regression tier (#251). Six cases over
// fixed in-memory fixtures, none of which needs a model or the network:
//
//   ttfp_inspector_fast_path  a 200-page text PDF streamed through
//                             StreamProcessDocument against a fake
//                             PdfBackendService and a fake pdf inspector:
//                             ns/op is the first PageData's arrival, items/s
//                             the pages per second over the whole stream
//   assembly_200_pages        append_page_data over 200 recognized pages of
//                             60 lines each
//   fold_and_delta            PageFold::fold over those pages' events, then
//                             repair_delta against the grouped, repaired
//                             document
//   render_exports            render_markdown, render_canonical_json and
//                             render_html over that document
//   sniff_corpus              resolve_mimetype over every file in
//                             tests/golden/corpus
//   base64_10mib              a 10 MiB encode and decode round trip
//
// Each case prints "name ns/op items/s" and its ratio to
// tests/bench/baseline.textproto. Timing is hand-rolled on steady_clock, like
// the rest of the suite: one warm-up operation, then batches sized to run at
// least kBatchTarget each, and the median batch is the number.
//
//   grparse-bench                      measure and compare; exit 77 (ctest's
//                                      skip) unless GRPARSE_BENCH_ENFORCE=1,
//                                      which turns a case slower than
//                                      baseline x 1.5 into a failure
//   grparse-bench --record --reason R  measure and rewrite the baseline
//   --baseline PATH                    another baseline file
//   --corpus DIR                       another sniff corpus
//
// GRPARSE_BENCH_REPORT=<path> also writes the measured run there in the
// baseline's own format, so a nightly's numbers can be committed as a new
// baseline unchanged.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <google/protobuf/text_format.h>
#include <grpcpp/grpcpp.h>

#include "ai/pipestream/parse/v1/parse_stream.grpc.pb.h"
#include "bench/bench_cases.h"
#include "bench/bench_support.h"
#include "bench_baseline.pb.h"
#include "grparse/base64.h"
#include "grparse/content_sniff.h"
#include "grparse/document_assembly.h"
#include "grparse/document_merge.h"
#include "grparse/document_parser_service.h"
#include "grparse/document_render.h"
#include "grparse/document_repair.h"
#include "grparse/page_scheduler.h"
#include "grparse/stream_delta.h"
#include "support/fake_pdf_backend.h"

#ifndef GRPARSE_BENCH_SOURCE_DIR
#error "GRPARSE_BENCH_SOURCE_DIR names the source tree (CMakeLists.txt sets it)"
#endif

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace bench = grparse::test::bench;
namespace parsev1 = ai::pipestream::parse::v1;
using grparse_bench::docv1::Document;

constexpr auto kBatchTarget = 100ms;
constexpr int kBatches = 7;
// Stream runs for the fast-path case, after one warm-up.
constexpr int kStreamRuns = 9;

// Keeps every result observable so no operation is optimized away.
volatile std::size_t sink = 0;

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values.at(values.size() / 2);
}

double nanoseconds(Clock::duration elapsed) {
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

bench::BenchCase result(std::string_view name, std::string_view item, double ns_per_op,
                        double items_per_op) {
  bench::BenchCase one;
  one.set_name(std::string(name));
  one.set_ns_per_op(ns_per_op);
  one.set_items_per_second(items_per_op * 1e9 / ns_per_op);
  one.set_item(std::string(item));
  return one;
}

// One warm-up, a batch size doubled until a batch takes kBatchTarget, then
// the median of kBatches such batches.
bench::BenchCase measure(std::string_view name, std::string_view item, double items_per_op,
                         const std::function<void()>& op) {
  op();
  const auto batch = [&op](int iterations) {
    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) op();
    return Clock::now() - start;
  };
  int iterations = 1;
  while (batch(iterations) < kBatchTarget && iterations < (1 << 20)) iterations *= 2;
  std::vector<double> per_op;
  for (int b = 0; b < kBatches; ++b) per_op.push_back(nanoseconds(batch(iterations)) / iterations);
  return result(name, item, median(per_op), items_per_op);
}

// ---- fixtures ---------------------------------------------------------------

std::vector<grparse::OcrPage> recognized_pages() {
  std::vector<grparse::OcrPage> pages;
  for (int page = 1; page <= grparse_bench::kPages; ++page) {
    pages.push_back(grparse_bench::synthetic_ocr_page(page));
  }
  return pages;
}

std::vector<parsev1::PageData> assembled_pages(const std::vector<grparse::OcrPage>& pages) {
  std::vector<parsev1::PageData> out(pages.size());
  grparse::AssemblyCursor cursor;
  for (size_t index = 0; index < pages.size(); ++index) {
    grparse::append_page_data(pages[index], static_cast<int>(index) + 1, &cursor, &out[index]);
  }
  return out;
}

// What the stream does once the CV pages are out (emit_fold_delta_locked):
// the fold with the terminal event's heading levels, then list grouping and
// the repair pass under a rename log.
struct Settled {
  std::map<std::string, int32_t> levels;
  Document after;
  std::map<std::string, std::string> renamed;
};

Settled settle(const std::vector<parsev1::PageData>& pages) {
  Settled settled;
  std::vector<grparse::HeaderHeight> headers;
  grparse::PageFold fold;
  for (const auto& page : pages) {
    grparse::collect_header_heights(page, &headers);
    fold.fold(page);
  }
  settled.levels = grparse::section_header_levels(std::move(headers));
  settled.after = fold.document();
  grparse::apply_section_header_levels(settled.levels, &settled.after);
  {
    const grparse::ReferenceRenameLog log;
    grparse::group_list_items(&settled.after);
    grparse::repair_document(&settled.after, grparse::RepairOptions{});
    settled.renamed = log.renames();
  }
  return settled;
}

// ---- cases ------------------------------------------------------------------

bench::BenchCase ttfp_inspector_fast_path() {
  static constexpr std::string_view kPdf = "%PDF-1.7\n% grparse-bench: 200 text pages\n";
  grparse_test::ScopedPdfBackend pdf_backend;
  std::vector<grparse_test::FakePdfPage> backend_pages;
  for (int page = 1; page <= grparse_bench::kPages; ++page) {
    backend_pages.push_back(grparse_test::text_page(
        {"Section " + std::to_string(page), grparse_bench::prose(page, 1)}));
  }
  pdf_backend.backend().add_document(std::string(kPdf), std::move(backend_pages));
  grparse_bench::PagedPdfInspector inspector;
  grparse_bench::InspectorServer inspector_server(&inspector);
  grparse_bench::IdleRecognizer recognizer;
  grparse::PageScheduler scheduler(recognizer, grparse::PageScheduler::Options{});
  grparse::CollectorTargets targets;
  targets.pdf = inspector_server.target();
  grparse::DocumentStreamingService service(
      scheduler, std::make_shared<grparse::CollectorEndpoints>(targets));
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  require(server != nullptr && port != 0, "bench stream server failed to start");
  auto stub = parsev1::ParseStreamingService::NewStub(
      grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));

  struct Run {
    double first_page_ns = 0;
    double total_ns = 0;
  };
  const auto run = [&stub]() {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 60s);
    parsev1::DocumentChunk chunk;
    chunk.set_document_id("bench-fast-path");
    chunk.set_filename("bench.pdf");
    chunk.set_content_type("application/pdf");
    chunk.set_data(std::string(kPdf));
    chunk.set_complete(true);
    const auto start = Clock::now();
    auto stream = stub->StreamProcessDocument(&context);
    require(stream->Write(chunk), "the bench client could not write its chunk");
    stream->WritesDone();
    Run timing;
    int pages = 0;
    parsev1::DocumentStreamEvent event;
    while (stream->Read(&event)) {
      if (!event.has_page()) continue;
      if (pages++ == 0) timing.first_page_ns = nanoseconds(Clock::now() - start);
    }
    const grpc::Status status = stream->Finish();
    timing.total_ns = nanoseconds(Clock::now() - start);
    require(status.ok(), "the fast-path stream failed: " + status.error_message());
    require(pages == grparse_bench::kPages,
            "the fast path streamed " + std::to_string(pages) + " pages, not " +
                std::to_string(grparse_bench::kPages));
    return timing;
  };
  run();
  std::vector<double> first_page;
  std::vector<double> total;
  for (int i = 0; i < kStreamRuns; ++i) {
    const Run timing = run();
    first_page.push_back(timing.first_page_ns);
    total.push_back(timing.total_ns);
  }
  server->Shutdown(std::chrono::system_clock::now() + 2s);
  server->Wait();
  require(recognizer.calls.load() == 0, "the fast path reached the recognizer");
  bench::BenchCase one = result("ttfp_inspector_fast_path", "pages", median(first_page), 1);
  one.set_items_per_second(grparse_bench::kPages * 1e9 / median(total));
  return one;
}

bench::BenchCase assembly_200_pages(const std::vector<grparse::OcrPage>& pages) {
  return measure("assembly_200_pages", "pages", static_cast<double>(pages.size()), [&pages] {
    grparse::AssemblyCursor cursor;
    for (size_t index = 0; index < pages.size(); ++index) {
      parsev1::PageData data;
      grparse::append_page_data(pages[index], static_cast<int>(index) + 1, &cursor, &data);
      sink = sink + static_cast<std::size_t>(data.texts_size());
    }
  });
}

bench::BenchCase fold_and_delta(const std::vector<parsev1::PageData>& pages,
                                const Settled& settled) {
  return measure("fold_and_delta", "pages", static_cast<double>(pages.size()), [&] {
    grparse::PageFold fold;
    for (const auto& page : pages) fold.fold(page);
    Document before = fold.document();
    grparse::apply_section_header_levels(settled.levels, &before);
    const auto delta =
        grparse::repair_delta(before, settled.after, settled.renamed, parsev1::COLLECTOR_GRPARSE_CV);
    sink = sink + (delta.has_value() ? static_cast<std::size_t>(delta->texts_size()) + 1 : 0);
  });
}

bench::BenchCase render_exports(const Document& document) {
  return measure("render_exports", "pages", grparse_bench::kPages, [&document] {
    sink = sink + grparse::render_markdown(document).size() +
           grparse::render_canonical_json(document).size() + grparse::render_html(document).size();
  });
}

bench::BenchCase sniff_corpus(const std::filesystem::path& corpus) {
  std::vector<std::pair<std::filesystem::path, std::string>> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(corpus)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    std::ostringstream bytes;
    bytes << in.rdbuf();
    files.emplace_back(entry.path().filename(), bytes.str());
  }
  std::sort(files.begin(), files.end());
  require(!files.empty(), "no files under the sniff corpus " + corpus.string());
  return measure("sniff_corpus", "files", static_cast<double>(files.size()), [&files] {
    for (const auto& [name, bytes] : files) {
      sink = sink + grparse::resolve_mimetype("", bytes, name).mimetype.size();
    }
  });
}

bench::BenchCase base64_10mib() {
  constexpr std::size_t kSize = 10U << 20U;
  std::string bytes(kSize, '\0');
  uint64_t state = 0x9E3779B97F4A7C15ULL;
  for (char& byte : bytes) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    byte = static_cast<char>(state >> 56U);
  }
  require(grparse::decode_base64(grparse::encode_base64(bytes.data(), bytes.size())) == bytes,
          "the base64 round trip changed the bytes");
  return measure("base64_10mib", "MiB", 10.0, [&bytes] {
    const std::string encoded = grparse::encode_base64(bytes.data(), bytes.size());
    sink = sink + grparse::decode_base64(encoded).size();
  });
}

// ---- baseline ---------------------------------------------------------------

std::string host_description() {
  std::string model = "unknown CPU";
  std::ifstream cpuinfo("/proc/cpuinfo");
  for (std::string line; std::getline(cpuinfo, line);) {
    if (line.starts_with("model name")) {
      const auto value = line.find_first_not_of(' ', line.find(':') + 1);
      if (line.find(':') != std::string::npos && value != std::string::npos) {
        model = line.substr(value);
      }
      break;
    }
  }
  return model + ", " + std::to_string(std::thread::hardware_concurrency()) + " hardware threads";
}

void write_textproto(const std::filesystem::path& path, const bench::BenchBaseline& baseline) {
  std::string text;
  require(google::protobuf::TextFormat::PrintToString(baseline, &text),
          "could not print the baseline");
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "# proto-file: tests/bench/bench_baseline.proto\n"
         "# proto-message: grparse.test.bench.BenchBaseline\n"
         "# Written by grparse-bench --record --reason \"<commit>: why\" (README,\n"
         "# \"Benchmarks\"); re-record instead of editing the numbers by hand.\n"
      << text;
  require(out.good(), "could not write " + path.string());
}

std::optional<bench::BenchBaseline> read_baseline(const std::filesystem::path& path,
                                                  std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = "cannot read " + path.string();
    return std::nullopt;
  }
  std::ostringstream text;
  text << in.rdbuf();
  bench::BenchBaseline baseline;
  if (!google::protobuf::TextFormat::ParseFromString(text.str(), &baseline)) {
    *error = path.string() + " is not a BenchBaseline in text format";
    return std::nullopt;
  }
  return baseline;
}

struct Arguments {
  bool record = false;
  std::string reason;
  std::filesystem::path baseline =
      std::filesystem::path(GRPARSE_BENCH_SOURCE_DIR) / "tests/bench/baseline.textproto";
  std::filesystem::path corpus = std::filesystem::path(GRPARSE_BENCH_SOURCE_DIR) / "tests/golden/corpus";
};

std::optional<Arguments> parse_arguments(int argc, char** argv) {
  Arguments arguments;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    const bool has_value = i + 1 < argc;
    if (argument == "--record") {
      arguments.record = true;
    } else if (argument == "--reason" && has_value) {
      arguments.reason = argv[++i];
    } else if (argument == "--baseline" && has_value) {
      arguments.baseline = argv[++i];
    } else if (argument == "--corpus" && has_value) {
      arguments.corpus = argv[++i];
    } else {
      return std::nullopt;
    }
  }
  if (arguments.record && arguments.reason.empty()) return std::nullopt;
  return arguments;
}

bool enforced() {
  const char* value = std::getenv("GRPARSE_BENCH_ENFORCE");
  return value != nullptr && std::string_view(value) == "1";
}

int run(const Arguments& arguments) {
  bench::BenchBaseline measured;
  measured.set_host(host_description());
  const auto add = [&measured](bench::BenchCase one) { *measured.add_cases() = std::move(one); };
  add(ttfp_inspector_fast_path());
  const std::vector<grparse::OcrPage> pages = recognized_pages();
  add(assembly_200_pages(pages));
  const std::vector<parsev1::PageData> events = assembled_pages(pages);
  const Settled settled = settle(events);
  add(fold_and_delta(events, settled));
  add(render_exports(settled.after));
  add(sniff_corpus(arguments.corpus));
  add(base64_10mib());
  for (size_t index = 0; index < grparse_bench::kCaseNames.size(); ++index) {
    require(measured.cases(static_cast<int>(index)).name() == grparse_bench::kCaseNames[index],
            "the bench ran its cases out of the order bench_cases.h names");
  }

  if (const char* report = std::getenv("GRPARSE_BENCH_REPORT"); report != nullptr && *report) {
    bench::BenchBaseline copy = measured;
    copy.set_reason("measured run (GRPARSE_BENCH_REPORT); not a recorded baseline");
    write_textproto(report, copy);
  }

  if (arguments.record) {
    measured.set_reason(arguments.reason);
    for (const auto& one : measured.cases()) {
      std::println("{:<26} {:>14.0f} ns/op {:>14.1f} items/s ({})", one.name(), one.ns_per_op(),
                   one.items_per_second(), one.item());
    }
    write_textproto(arguments.baseline, measured);
    std::println("grparse-bench: recorded {} cases to {}", measured.cases_size(),
                 arguments.baseline.string());
    return EXIT_SUCCESS;
  }

  const bool enforce = enforced();
  std::string error;
  const auto baseline = read_baseline(arguments.baseline, &error);
  std::map<std::string, bench::BenchCase> recorded;
  if (baseline.has_value()) {
    for (const auto& one : baseline->cases()) recorded[one.name()] = one;
  } else {
    std::println(stderr, "grparse-bench: {}", error);
  }
  int slower = 0;
  for (const auto& one : measured.cases()) {
    std::string verdict = "no baseline";
    const auto found = recorded.find(one.name());
    if (found != recorded.end() && found->second.ns_per_op() > 0 &&
        found->second.items_per_second() > 0) {
      const double time_ratio = one.ns_per_op() / found->second.ns_per_op();
      const double rate_ratio = found->second.items_per_second() / one.items_per_second();
      const bool slow = time_ratio > grparse_bench::kSlowdownLimit ||
                        rate_ratio > grparse_bench::kSlowdownLimit;
      verdict = std::format("{:.2f}x ns/op, {:.2f}x items/s of baseline{}", time_ratio,
                            1.0 / rate_ratio, slow ? "  SLOWER than baseline x 1.5" : "");
      if (slow) ++slower;
    } else {
      ++slower;
    }
    std::println("{:<26} {:>14.0f} ns/op {:>14.1f} items/s ({})  {}", one.name(), one.ns_per_op(),
                 one.items_per_second(), one.item(), verdict);
  }
  if (!enforce) {
    std::println("grparse-bench: not enforced (GRPARSE_BENCH_ENFORCE=1 gates on the baseline); "
                 "{} case(s) slower than baseline x {} or without one",
                 slower, grparse_bench::kSlowdownLimit);
    return 77;
  }
  if (slower > 0) {
    std::println(stderr, "grparse-bench: {} case(s) slower than baseline x {} or without one",
                 slower, grparse_bench::kSlowdownLimit);
    return EXIT_FAILURE;
  }
  std::println("grparse-bench: every case within baseline x {}", grparse_bench::kSlowdownLimit);
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
  const auto arguments = parse_arguments(argc, argv);
  if (!arguments.has_value()) {
    std::println(stderr,
                 "usage: grparse-bench [--baseline PATH] [--corpus DIR] "
                 "[--record --reason \"<commit>: why\"]");
    return 2;
  }
  try {
    return run(*arguments);
  } catch (const std::exception& failure) {
    std::println(stderr, "grparse-bench: {}", failure.what());
    return EXIT_FAILURE;
  }
}
