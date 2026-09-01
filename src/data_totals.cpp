#include "grparse/data_totals.h"

#include <cstdlib>
#include <print>
#include <string>

namespace grparse {

DataCounters& data_counters() {
  static DataCounters counters;
  return counters;
}

DataTotals data_totals() {
  const DataCounters& live = data_counters();
  DataTotals totals;
  totals.charts_bound = live.charts_bound.load(std::memory_order_relaxed);
  totals.chart_captions = live.chart_captions.load(std::memory_order_relaxed);
  totals.sheet_header_rows = live.sheet_header_rows.load(std::memory_order_relaxed);
  totals.mimetypes_sniffed = live.mimetypes_sniffed.load(std::memory_order_relaxed);
  totals.cv_enrichment_skipped =
      live.cv_enrichment_skipped.load(std::memory_order_relaxed);
  totals.charts_derendered = live.charts_derendered.load(std::memory_order_relaxed);
  totals.chart_derender_skipped =
      live.chart_derender_skipped.load(std::memory_order_relaxed);
  return totals;
}

bool data_log_enabled() {
  static const bool enabled = [] {
    const char* configured = std::getenv("GRPARSE_DATA_LOG");
    return configured != nullptr && std::string(configured) == "on";
  }();
  return enabled;
}

void data_log(std::string_view line) {
  if (!data_log_enabled()) return;
  std::println("gRParse data: {}", line);
}

}  // namespace grparse
