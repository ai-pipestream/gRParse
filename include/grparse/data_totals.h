#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace grparse {

// Process-wide counts of the data-plane changes this service makes to the
// documents it hands out, exported next to the repair totals. Each count
// names one change a reader could verify on the wire: a chart that came
// with a bound data table, a caption minted from a chart title, a sheet
// row marked as a column header, a mimetype that came from the bytes
// instead of the name, and a spreadsheet whose page renders were kept away
// from the CV detector.
struct DataTotals {
  uint64_t charts_bound = 0;
  uint64_t chart_captions = 0;
  uint64_t sheet_header_rows = 0;
  uint64_t mimetypes_sniffed = 0;
  uint64_t cv_enrichment_skipped = 0;
};

// The live counters behind data_totals(). Increments are relaxed atomics:
// the totals are a monitoring surface, never a synchronisation point.
struct DataCounters {
  std::atomic<uint64_t> charts_bound{0};
  std::atomic<uint64_t> chart_captions{0};
  std::atomic<uint64_t> sheet_header_rows{0};
  std::atomic<uint64_t> mimetypes_sniffed{0};
  std::atomic<uint64_t> cv_enrichment_skipped{0};
};

DataCounters& data_counters();

// A snapshot of the counters.
DataTotals data_totals();

// True when GRPARSE_DATA_LOG=on asked for one stdout line per data-plane
// change (the same opt-in shape as GRPARSE_REPAIR=debug). Read once.
bool data_log_enabled();

// One "gRParse data: ..." stdout line when data_log_enabled().
void data_log(std::string_view line);

}  // namespace grparse
