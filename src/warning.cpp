#include "grparse/warning.h"

namespace grparse {

void stamp_warning(const Warning& warning, std::string_view collector,
                   ai::pipestream::document::v1::CollectorWarning* out) {
  out->Clear();
  out->mutable_source()->set_collector(std::string(collector));
  out->set_message(warning.message);
  out->set_code(warning.code);
  if (warning.page_no.has_value()) out->set_page_no(*warning.page_no);
  if (!warning.item_ref.empty()) out->set_item_ref(warning.item_ref);
  if (warning.bytes_kept.has_value()) out->set_bytes_kept(*warning.bytes_kept);
  if (warning.bytes_total.has_value()) out->set_bytes_total(*warning.bytes_total);
  if (warning.count.has_value()) out->set_count(*warning.count);
}

}  // namespace grparse
