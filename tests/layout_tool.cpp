// Development utility for layout and figure-classification measurement: a PDF
// or raster image in, per-page detections and timings out.  Built with the
// tests, never installed or staged into images.
//
// It runs exactly the engines the server runs, on the execution provider the
// usual GRPARSE_ORT_EP / GRPARSE_OPENVINO_DEVICE / GRPARSE_CUDA_DEVICE
// settings select, so a run here is comparable to a run of the service - with
// none of the gRPC, OCR, or assembly work in the measurement.
//
//   grparse-layout-tool <document> [--models DIR] [--threads N] [--repeat N]
//                       [--sessions N]
//
// --threads drives the shared layout session from N workers at once, which is
// what the pipeline does; --repeat runs the whole document that many times so
// warm-up is separable from steady state.  --sessions holds N layout sessions
// open instead of the one the server uses, which is how the memory a
// session-per-worker pool would cost gets measured rather than guessed.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "grparse/figure_classifier.h"
#include "grparse/in_memory_document.h"
#include "grparse/layout_engine.h"
#include "grparse_session_ep.h"

namespace {

namespace fs = std::filesystem;

std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read " + path.string());
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

double milliseconds_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

// The provider selection the service makes, so the tool measures the same
// thing the service would.  Unset means CPU.
grparse::OrtEp selected_provider() {
  const char* configured = std::getenv("GRPARSE_ORT_EP");
  const std::string selection =
      configured == nullptr || *configured == '\0' ? "cpu" : configured;
  if (selection == "cuda") return grparse::OrtEp::kCuda;
  if (selection == "openvino") return grparse::OrtEp::kOpenVino;
  if (selection == "cpu") return grparse::OrtEp::kCpu;
  throw std::invalid_argument("GRPARSE_ORT_EP must be cuda, openvino, or cpu for this tool");
}

std::string provider_name(grparse::OrtEp ep) {
  switch (ep) {
    case grparse::OrtEp::kCuda: return "cuda";
    case grparse::OrtEp::kOpenVino: return "openvino";
    case grparse::OrtEp::kCpu: break;
  }
  return "cpu";
}

// Peak resident set size of this process, in kilobytes; the kernel's own
// high-water mark, so a transient spike during model load still shows.
long peak_rss_kb() {
  std::ifstream status("/proc/self/status");
  std::string key;
  while (status >> key) {
    if (key == "VmHWM:") {
      long value = 0;
      status >> value;
      return value;
    }
    status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }
  return 0;
}

struct PageResult {
  int page_number = 0;
  double layout_ms = 0.0;
  std::vector<grparse::LayoutRegion> regions;
};

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      std::println(stderr,
                   "usage: grparse-layout-tool <document> [--models DIR] [--threads N] "
                   "[--repeat N] [--sessions N]");
      return EXIT_FAILURE;
    }
    const fs::path document = argv[1];
    fs::path models_dir = "models";
    size_t threads = 1;
    size_t repeats = 1;
    size_t sessions = 1;
    for (int index = 2; index + 1 < argc; index += 2) {
      const std::string flag = argv[index];
      const std::string value = argv[index + 1];
      if (flag == "--models") models_dir = value;
      else if (flag == "--threads") threads = std::max<size_t>(1, std::stoul(value));
      else if (flag == "--repeat") repeats = std::max<size_t>(1, std::stoul(value));
      else if (flag == "--sessions") sessions = std::max<size_t>(1, std::stoul(value));
      else throw std::invalid_argument("unknown flag " + flag);
    }

    const grparse::OrtEp ep = selected_provider();
    const char* openvino_device = std::getenv("GRPARSE_OPENVINO_DEVICE");
    const char* cuda_device = std::getenv("GRPARSE_CUDA_DEVICE");
    grparse::OrtEpSelection selection{
        .ep = ep,
        .cuda_device = cuda_device == nullptr || *cuda_device == '\0' ? 0 : std::stoi(cuda_device),
        .openvino_device = openvino_device == nullptr || *openvino_device == '\0'
                               ? "GPU"
                               : openvino_device,
    };
    grparse::set_ort_ep_selection(selection);
    // The pooled sessions divide the machine exactly as the server's do, so a
    // measurement here is a measurement of the server.
    const unsigned hardware = std::max(1U, std::thread::hardware_concurrency());
    grparse::set_ort_intra_op_threads(
        static_cast<int>(std::max<size_t>(1, hardware / threads)));

    const grparse::LayoutModel model = grparse::configured_layout_model();
    const fs::path layout_path = models_dir / grparse::layout_model_file(model);
    const auto load_started = std::chrono::steady_clock::now();
    grparse::LayoutEngine layout(layout_path, model);
    const double layout_load_ms = milliseconds_since(load_started);

    // Extra sessions exist only to be paid for: holding them open is how the
    // memory a session-per-worker pool would have cost gets measured.
    std::vector<std::unique_ptr<grparse::LayoutEngine>> extra_sessions;
    for (size_t index = 1; index < sessions; ++index) {
      extra_sessions.push_back(std::make_unique<grparse::LayoutEngine>(layout_path, model));
    }

    const fs::path classifier_path = models_dir / "figure_classifier.onnx";
    std::unique_ptr<grparse::FigureClassifierPool> classifier;
    if (fs::exists(classifier_path)) {
      classifier = std::make_unique<grparse::FigureClassifierPool>(classifier_path, threads);
    }

    std::println("provider={} openvino_device={} cuda_device={} layout_model={} labels={} "
                 "load_ms={:.1f} sessions={} rss_peak_kb={} cores={} pooled_intra_op={} "
                 "layout_intra_op=all",
                 provider_name(ep), selection.openvino_device, selection.cuda_device,
                 grparse::layout_model_name(model), layout.labels().size(), layout_load_ms,
                 sessions, peak_rss_kb(), hardware, grparse::ort_intra_op_threads());

    const std::string bytes = read_file(document);
    const bool pdf = document.extension() == ".pdf" || document.extension() == ".PDF";
    auto source = grparse::open_in_memory_document(
        std::make_shared<const std::string>(bytes), pdf, threads);
    const int pages = source->page_count();
    std::println("document={} pages={} threads={} repeats={}", document.string(), pages, threads,
                 repeats);

    // Rasterize once: the measurement is the device call, not Poppler.
    std::vector<cv::Mat> rasters;
    rasters.reserve(static_cast<size_t>(pages));
    for (int page = 1; page <= pages; ++page) rasters.push_back(source->render_page(page));

    std::vector<PageResult> results(static_cast<size_t>(pages) * repeats);
    std::atomic<size_t> next{0};
    const auto wall_started = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < threads; ++worker) {
      workers.emplace_back([&] {
        while (true) {
          const size_t job = next.fetch_add(1);
          if (job >= results.size()) return;
          const size_t page_index = job % static_cast<size_t>(pages);
          const auto started = std::chrono::steady_clock::now();
          auto regions = layout.detect_regions(rasters[page_index]);
          const double elapsed = milliseconds_since(started);
          results[job] = PageResult{static_cast<int>(page_index) + 1, elapsed, std::move(regions)};
        }
      });
    }
    for (auto& worker : workers) worker.join();
    const double wall_ms = milliseconds_since(wall_started);

    std::map<std::string, size_t> label_counts;
    std::vector<double> timings;
    timings.reserve(results.size());
    for (const auto& result : results) {
      timings.push_back(result.layout_ms);
      for (const auto& region : result.regions) ++label_counts[region.label];
    }
    std::ranges::sort(timings);
    const double median = timings[timings.size() / 2];
    const double total = std::accumulate(timings.begin(), timings.end(), 0.0);

    for (size_t index = 0; index < static_cast<size_t>(pages); ++index) {
      const auto& first_pass = results[index];
      std::println("page {} layout_ms={:.1f} regions={}", first_pass.page_number,
                   first_pass.layout_ms, first_pass.regions.size());
      for (const auto& region : first_pass.regions) {
        std::println("  {:<20} {:.4f} [{}, {}, {}, {}]", region.label, region.confidence,
                     region.left, region.top, region.right, region.bottom);
      }
    }

    std::println("layout timings ms: min={:.1f} median={:.1f} max={:.1f} mean={:.1f} wall={:.1f} "
                 "peak_concurrency={}",
                 timings.front(), median, timings.back(), total / static_cast<double>(timings.size()),
                 wall_ms, layout.stats().peak_concurrency);
    std::string labels;
    for (const auto& [label, count] : label_counts) {
      if (!labels.empty()) labels += ", ";
      labels += label + "=" + std::to_string(count);
    }
    std::println("labels: {}", labels);

    if (classifier != nullptr) {
      size_t classified = 0;
      for (size_t index = 0; index < static_cast<size_t>(pages); ++index) {
        for (const auto& region : results[index].regions) {
          if (region.label != "picture") continue;
          const cv::Rect roi(region.left, region.top, std::max(1, region.right - region.left),
                             std::max(1, region.bottom - region.top));
          const cv::Rect clipped = roi & cv::Rect(0, 0, rasters[index].cols, rasters[index].rows);
          if (clipped.width <= 1 || clipped.height <= 1) continue;
          const auto started = std::chrono::steady_clock::now();
          const auto classes = classifier->classify(rasters[index](clipped));
          const double elapsed = milliseconds_since(started);
          ++classified;
          std::println("page {} picture [{}, {}, {}, {}] classify_ms={:.1f} top={} ({:.4f}) "
                       "second={} ({:.4f}) classes={}",
                       results[index].page_number, clipped.x, clipped.y, clipped.br().x,
                       clipped.br().y, elapsed, classes[0].label, classes[0].confidence,
                       classes[1].label, classes[1].confidence, classes.size());
        }
      }
      std::println("pictures classified: {}", classified);
    } else {
      std::println("figure classifier: absent at {}", classifier_path.string());
    }
    std::println("rss_peak_kb={} sessions={}", peak_rss_kb(), sessions);
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "grparse-layout-tool: {}", error.what());
    return EXIT_FAILURE;
  }
}
