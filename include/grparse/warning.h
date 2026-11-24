#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ai/pipestream/document/v1/document.pb.h"

namespace grparse {

// One warning on its way to the caller: the words a person reads, plus what
// a consumer acts on (the code, and the page and item it is about, where the
// warning is about one). It is the in-process form of
// document.v1.CollectorWarning without the source, which the leg that
// carries the warning supplies when it stamps it.
//
// A warning built from a string alone is a collector's own free-text note:
// it keeps WARNING_CODE_UNSPECIFIED. Anything gRParse itself decides (a
// fallback, a skipped part, a cut-off read) states its code.
struct Warning {
  std::string message;
  ai::pipestream::document::v1::WarningCode code =
      ai::pipestream::document::v1::WARNING_CODE_UNSPECIFIED;
  // 1-based, as Document.pages keys are.
  std::optional<int32_t> page_no;
  // The self_ref of the item the warning is about; empty when it is about
  // no single item.
  std::string item_ref;
  // For content that was cut: how much was kept, and how much the source
  // held when that is known.
  std::optional<uint64_t> bytes_kept;
  std::optional<uint64_t> bytes_total;
  // How many occurrences this one warning stands for, when repeats of the
  // same finding were folded into it; unset means one.
  std::optional<uint64_t> count;
  // For a recording that was cut: how much of its timeline is in the
  // document and how long the media says it is, in milliseconds.
  std::optional<uint64_t> duration_kept_ms;
  std::optional<uint64_t> duration_total_ms;

  Warning() = default;
  // Implicit on purpose: a free-text warning is still a warning, and every
  // collector that passes a peer's words on verbatim has only the words.
  Warning(std::string text) : message(std::move(text)) {}  // NOLINT(google-explicit-constructor)
  Warning(const char* text) : message(text) {}             // NOLINT(google-explicit-constructor)
  Warning(ai::pipestream::document::v1::WarningCode warning_code, std::string text)
      : message(std::move(text)), code(warning_code) {}

  // The same warning, about one page.
  Warning on_page(int32_t page) && {
    page_no = page;
    return std::move(*this);
  }
  // The same warning, about one item.
  Warning on_item(std::string self_ref) && {
    item_ref = std::move(self_ref);
    return std::move(*this);
  }

  // The same warning, with how much of the content was kept.
  Warning kept(uint64_t kept_bytes, std::optional<uint64_t> total_bytes = std::nullopt) && {
    bytes_kept = kept_bytes;
    bytes_total = total_bytes;
    return std::move(*this);
  }

  // The same warning, with how much of the recording's timeline was kept.
  Warning kept_duration(uint64_t kept_ms,
                        std::optional<uint64_t> total_ms = std::nullopt) && {
    duration_kept_ms = kept_ms;
    duration_total_ms = total_ms;
    return std::move(*this);
  }

  // The same warning, standing for this many occurrences of its finding.
  Warning times(uint64_t occurrences) && {
    count = occurrences;
    return std::move(*this);
  }

  friend bool operator==(const Warning&, const Warning&) = default;
};

// Writes `warning` into its wire form under `collector` (the name the
// collector's items carry in CollectorSource.collector). Every field the
// warning states is written; none is derived from the message text.
void stamp_warning(const Warning& warning, std::string_view collector,
                   ai::pipestream::document::v1::CollectorWarning* out);

}  // namespace grparse
