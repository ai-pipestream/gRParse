// The in-process warning and its wire form: every field a warning states
// reaches document.v1.CollectorWarning, and a warning that is only words
// claims no code, page or item.

#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/warning.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

using grparse::Warning;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

void verify_words_alone_claim_no_code_page_or_item() {
  const Warning warning = std::string("rtf body skipped");
  docv1::CollectorWarning wire;
  grparse::stamp_warning(warning, "libreoffice", &wire);
  require_equal(wire.source().collector(), "libreoffice", "the leg that carried it is the source");
  require_equal(wire.message(), "rtf body skipped", "the words pass through verbatim");
  require(wire.code() == docv1::WARNING_CODE_UNSPECIFIED,
          "a free-text warning has no typed meaning to claim");
  require(!wire.has_page_no(), "a warning about no page states no page");
  require(!wire.has_item_ref(), "a warning about no item states no item");
  require(!wire.has_bytes_kept() && !wire.has_bytes_total(),
          "a warning about nothing cut states no sizes");
  require(!wire.has_count(), "a warning that stands for one occurrence states no count");
}

void verify_a_folded_warning_keeps_its_count() {
  const Warning warning =
      Warning(docv1::WARNING_CODE_ENRICHMENT_CROP_MISSING, "no crop could be rendered").times(12);
  docv1::CollectorWarning wire;
  grparse::stamp_warning(warning, "grparse-cv", &wire);
  require(wire.has_count() && wire.count() == 12,
          "how many occurrences the warning stands for is written");
  require(!wire.has_page_no() && !wire.has_item_ref(),
          "a warning about the whole document names no page and no item");
}

void verify_a_typed_warning_keeps_its_code_page_and_item() {
  const Warning warning =
      Warning(docv1::WARNING_CODE_COLLECTOR_FALLBACK, "pdf collector failed")
          .on_page(7)
          .on_item("#/tables/3")
          .kept(65536, 1048576);
  docv1::CollectorWarning wire;
  grparse::stamp_warning(warning, "pdf", &wire);
  require(wire.code() == docv1::WARNING_CODE_COLLECTOR_FALLBACK, "the code is written");
  require(wire.has_page_no() && wire.page_no() == 7, "the page is written");
  require(wire.has_item_ref() && wire.item_ref() == "#/tables/3", "the item is written");
  require(wire.has_bytes_kept() && wire.bytes_kept() == 65536, "the kept size is written");
  require(wire.has_bytes_total() && wire.bytes_total() == 1048576,
          "the source size is written");
  require_equal(wire.message(), "pdf collector failed", "the words still ride beside the code");
}

void verify_stamping_replaces_whatever_the_message_held() {
  docv1::CollectorWarning wire;
  wire.set_page_no(2);
  wire.set_item_ref("#/texts/9");
  wire.set_code(docv1::WARNING_CODE_COLLECTOR_FALLBACK);
  grparse::stamp_warning(Warning("words only"), "calamine", &wire);
  require(wire.code() == docv1::WARNING_CODE_UNSPECIFIED && !wire.has_page_no() &&
              !wire.has_item_ref(),
          "a reused message keeps nothing of the warning it held before");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("warning-test", "ok", {
      verify_words_alone_claim_no_code_page_or_item,
      verify_a_typed_warning_keeps_its_code_page_and_item,
      verify_a_folded_warning_keeps_its_count,
      verify_stamping_replaces_whatever_the_message_held,
  });
}
