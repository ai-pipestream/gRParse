// S3 eval finding: a collector's source_meta.created arrived on the wire
// attributed as "created.seconds". The merge walked into the Timestamp as if
// it were a record of fields a collector answers one by one, so two
// collectors' instants could interleave seconds and nanos and the
// field_sources list named a path no schema reader knows. A well-known type
// is one value: taken or left whole, attributed under the field that holds
// it.

#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/document_merge.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

using grparse_test::require;

docv1::Document base_document() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_furniture()->set_self_ref("#/furniture");
  return document;
}

docv1::CollectorSource collector(const std::string& name) {
  docv1::CollectorSource source;
  source.set_collector(name);
  return source;
}

docv1::Document with_created(long long seconds, int nanos, const std::string& title) {
  docv1::Document document = base_document();
  document.mutable_source_meta()->set_title(title);
  document.mutable_source_meta()->mutable_created()->set_seconds(seconds);
  document.mutable_source_meta()->mutable_created()->set_nanos(nanos);
  return document;
}

const docv1::FieldSource* source_of(const docv1::DocumentMeta& meta, const std::string& field) {
  for (const auto& entry : meta.field_sources()) {
    if (entry.field() == field) return &entry;
  }
  return nullptr;
}

void verify_timestamp_is_attributed_as_one_field() {
  docv1::Document target = base_document();
  grparse::merge_documents(with_created(1'700'000'000, 0, "Report"), &target, collector("libreoffice"));
  const docv1::DocumentMeta& meta = target.source_meta();
  require(source_of(meta, "created") != nullptr &&
              source_of(meta, "created")->source().collector() == "libreoffice",
          "the instant is attributed under 'created'");
  require(source_of(meta, "created.seconds") == nullptr && source_of(meta, "created.nanos") == nullptr,
          "no path reaches inside the Timestamp");
  for (const auto& entry : meta.field_sources()) {
    require(entry.field().find('.') == std::string::npos ||
                entry.field().rfind("created", 0) != 0,
            "no attributed path names a Timestamp component: " + entry.field());
  }
}

// The second collector states a different instant with nanos; the incumbent
// keeps its whole value, the challenger's nanos never leak into it.
void verify_timestamp_is_taken_or_left_whole() {
  docv1::Document target = base_document();
  grparse::merge_documents(with_created(1'700'000'000, 0, "Report"), &target, collector("libreoffice"));
  grparse::merge_documents(with_created(1'600'000'000, 250, "Other"), &target, collector("poi"));
  const auto& created = target.source_meta().created();
  const bool first = created.seconds() == 1'700'000'000 && created.nanos() == 0;
  const bool second = created.seconds() == 1'600'000'000 && created.nanos() == 250;
  require(first || second, "the merged instant is one collector's instant, whole");
  const docv1::FieldSource* holder = source_of(target.source_meta(), "created");
  require(holder != nullptr, "the instant names its holder");
  require((first && holder->source().collector() == "libreoffice") ||
              (second && holder->source().collector() == "poi"),
          "the recorded holder is the collector whose instant is carried");
}

// claim_fields on a message already holding a Timestamp records the field,
// not its components: the service's own origin stamp path shares the walk.
void verify_claim_fields_stops_at_values() {
  docv1::DocumentMeta meta;
  meta.set_title("T");
  meta.mutable_modified()->set_seconds(42);
  grparse::claim_fields(&meta, collector("grparse"));
  require(source_of(meta, "modified") != nullptr, "the Timestamp field itself is claimed");
  require(source_of(meta, "modified.seconds") == nullptr, "its components are not");
  require(source_of(meta, "title") != nullptr, "scalars are still claimed");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("document_merge_value_fields_test", "ok", {
      verify_timestamp_is_attributed_as_one_field,
      verify_timestamp_is_taken_or_left_whole,
      verify_claim_fields_stops_at_values,
  });
}
