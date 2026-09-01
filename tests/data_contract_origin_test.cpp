// Anti-drift: the identity a document carries and the legs it turns off.
//
// Before any collector runs, the service stamps what it knows about the bytes
// on the document's origin: the resolved mimetype and the evidence that
// resolution rests on, attributed to the service itself so no collector's
// guess displaces it. Downstream readers treat those as typed fields, not as
// strings parsed out of a metadata bag, and the scorecard's agreement section
// counts them. This pins the ladder, the typed shape it lands in, the title a
// markup collector only recorded as metadata, and the enrichment leg a
// spreadsheet turns off.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <google/protobuf/util/message_differencer.h>

#include "ai/pipestream/document/v1/document.pb.h"
#include "grparse/confluence_storage.h"
#include "grparse/content_sniff.h"
#include "grparse/data_totals.h"
#include "grparse/document_collectors.h"
#include "grparse/document_merge.h"
#include "grparse/office_cv_enrichment.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;

namespace {

using grparse_test::require;

// The four words the resolution may rest on; anything else is a new
// vocabulary a reader was never told about.
const std::set<std::string> kEvidence = {"declared", "magic", "extension", "fallback"};

// The identity stamp exactly as the service builds it: resolve, write both
// fields, claim them for this service, and file the claim.
docv1::Document stamped_document(const std::string& declared, const std::string& bytes,
                                 const std::string& filename) {
  docv1::Document document;
  document.set_name(filename);
  auto* origin = document.mutable_origin();
  origin->set_filename(filename);
  const grparse::MimetypeResolution resolved =
      grparse::resolve_mimetype(declared, bytes, filename);
  origin->set_mimetype(resolved.mimetype);
  origin->set_mimetype_evidence(resolved.evidence);
  origin->set_binary_hash(0x0123456789ABCDEFULL);
  docv1::CollectorSource stamp;
  stamp.set_collector("grparse");
  grparse::claim_fields(origin, stamp);
  auto* claim = document.add_claims();
  *claim->mutable_source() = stamp;
  *claim->mutable_origin() = *origin;
  claim->mutable_origin()->clear_field_sources();
  return document;
}

// The ladder, rung by rung, read off the stamped origin rather than off the
// resolver's return value: the contract is what the document carries.
void verify_the_mimetype_ladder_is_stamped_on_the_origin() {
  struct Case {
    std::string declared;
    std::string bytes;
    std::string filename;
    std::string mimetype;
    std::string evidence;
    std::string why;
  };
  const std::string pdf = "%PDF-1.4\n";
  const std::string ole2 = std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
  const std::vector<Case> cases = {
      {"text/plain; charset=utf-8", pdf, "report.pdf", "text/plain", "declared",
       "an explicit request content type wins over the bytes"},
      {"application/octet-stream", pdf, "report.bin", "application/pdf", "magic",
       "octet-stream is no declaration, so the bytes decide"},
      {"", ole2, "old.doc", "application/msword", "extension",
       "bytes that name no format fall to the extension"},
      {"", std::string("\x01\x02\x03", 3), "blob.zzz", "application/octet-stream", "fallback",
       "nothing known is octet-stream"},
      {"", "<p>Body</p>", "page.storage.xhtml", grparse::kConfluenceStorageMimetype, "extension",
       "the one name that precedes the bytes is the storage dialect suffix"},
  };
  for (const Case& item : cases) {
    const docv1::Document document = stamped_document(item.declared, item.bytes, item.filename);
    const docv1::DocumentOrigin& origin = document.origin();
    require(origin.mimetype() == item.mimetype,
            item.why + ": expected " + item.mimetype + ", stamped " + origin.mimetype());
    require(origin.mimetype_evidence() == item.evidence,
            item.why + ": expected evidence " + item.evidence + ", stamped " +
                origin.mimetype_evidence());
    require(kEvidence.contains(origin.mimetype_evidence()),
            "evidence '" + origin.mimetype_evidence() + "' is outside the stated vocabulary");
    require(!origin.mimetype().empty(), "a stamped origin always names a mimetype");
  }
}

// The evidence is a typed field with a typed claim behind it, not a string
// stuffed into a metadata bag under a namespaced key.
void verify_the_evidence_rides_as_a_typed_claim() {
  const docv1::Document document = stamped_document("", "%PDF-1.4\n", "report.pdf");
  const docv1::DocumentOrigin& origin = document.origin();

  const auto* descriptor = docv1::DocumentOrigin::descriptor();
  require(descriptor->FindFieldByName("mimetype") != nullptr,
          "DocumentOrigin.mimetype is gone; the stamp has nowhere typed to land");
  require(descriptor->FindFieldByName("mimetype_evidence") != nullptr,
          "DocumentOrigin.mimetype_evidence is gone; the evidence would degrade to a string key");
  require(descriptor->FindFieldByName("field_sources") != nullptr,
          "DocumentOrigin.field_sources is gone; nothing could attribute the stamp");

  bool claims_mimetype = false;
  bool claims_evidence = false;
  for (const auto& source : origin.field_sources()) {
    if (source.field() == "mimetype") {
      claims_mimetype = source.source().collector() == "grparse";
    }
    if (source.field() == "mimetype_evidence") {
      claims_evidence = source.source().collector() == "grparse";
    }
  }
  require(claims_mimetype && claims_evidence,
          "the service does not claim the mimetype and its evidence as its own fields");

  require(document.claims_size() == 1, "the identity stamp files exactly one claim");
  const docv1::CollectorClaim& claim = document.claims(0);
  require(claim.source().collector() == "grparse", "the claim is the service's own");
  require(claim.origin().mimetype() == origin.mimetype() &&
              claim.origin().mimetype_evidence() == origin.mimetype_evidence(),
          "the claim carries the same resolution the origin does");
  require(claim.origin().field_sources_size() == 0,
          "the claim's copy of the origin carries no attribution of its own");
}

// A markup collector records the page's own title as metadata; the title item
// the heading tree needs is derived from it, once, and attributed to this
// service rather than claimed for the collector.
void verify_a_metadata_title_becomes_one_attributed_title_item() {
  docv1::Document document;
  document.mutable_body()->set_self_ref("#/body");
  document.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
  document.mutable_source_meta()->set_title("A Page With A Title");
  const std::string ref = "#/texts/0";
  auto* base = document.add_texts()->mutable_text()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text("The body opens here.");
  base->add_source()->mutable_collector()->set_collector("lol-html");
  document.mutable_body()->add_children()->set_ref(ref);

  require(grparse::promote_source_title(&document), "the metadata title was not promoted");
  require(document.texts_size() == 2, "the title is a new arena item");
  const docv1::BaseTextItem& title = document.texts(1);
  require(title.has_title(), "the promoted item is a TitleItem");
  require(title.title().base().text() == "A Page With A Title", "the title keeps its text");
  require(title.title().base().label() == docv1::DOC_ITEM_LABEL_TITLE, "the label is TITLE");
  require(title.title().base().source_size() == 1 &&
              title.title().base().source(0).collector().collector() == "grparse" &&
              title.title().base().source(0).collector().model() == "source-meta-title",
          "the derived title is attributed to this service, not to the collector");
  require(document.body().children(0).ref() == title.title().base().self_ref(),
          "the title leads the body");

  const docv1::Document once = document;
  require(!grparse::promote_source_title(&document), "a second promotion added another title");
  require(google::protobuf::util::MessageDifferencer::Equals(once, document),
          "a second promotion changed the document");

  docv1::Document silent;
  silent.mutable_body()->set_self_ref("#/body");
  require(!grparse::promote_source_title(&silent), "metadata with no title promoted something");
}

// A spreadsheet's page renders are cell grids the layout detector reads as
// one figure, so the service hands the office leg an enrichment with no
// engines at all. That skip has to be total: the document comes back
// untouched rather than half enriched.
void verify_the_disabled_enrichment_leg_is_a_total_no_op() {
  docv1::Document document;
  document.set_name("book.xlsx");
  document.mutable_body()->set_self_ref("#/body");
  auto& page = (*document.mutable_pages())[1];
  page.set_page_no(1);
  page.mutable_size()->set_width(24000);
  page.mutable_size()->set_height(15000);
  page.mutable_image()->set_mimetype("image/png");
  page.mutable_image()->set_uri("data:image/png;base64,iVBORw0KGgo=");
  const std::string ref = "#/tables/0";
  auto* table = document.add_tables();
  table->set_self_ref(ref);
  table->mutable_parent()->set_ref("#/body");
  table->set_label(docv1::DOC_ITEM_LABEL_TABLE);
  document.mutable_body()->add_children()->set_ref(ref);

  const docv1::Document before = document;
  const grparse::OfficeCvTotals totals_before = grparse::office_cv_totals();
  const grparse::OfficeCvReport report =
      grparse::enrich_office_document(grparse::OfficeCvEnrichment{}, &document);
  const grparse::OfficeCvTotals totals_after = grparse::office_cv_totals();

  require(report.pictures_added == 0 && report.pictures_anchored == 0 &&
              report.pictures_deduplicated == 0,
          "the disabled enrichment leg reported work");
  require(document.pictures_size() == 0, "the disabled enrichment leg added a picture");
  require(google::protobuf::util::MessageDifferencer::Equals(before, document),
          "the disabled enrichment leg changed the document");
  require(totals_after.pictures_added == totals_before.pictures_added &&
              totals_after.pictures_anchored == totals_before.pictures_anchored,
          "the disabled enrichment leg moved the process totals");

  // The decision itself is taken in the service, which picks the empty
  // enrichment for a spreadsheet and records the choice. That record has to
  // reach the exported totals, or a skipped leg is invisible to a reader.
  const std::uint64_t skipped = grparse::data_totals().cv_enrichment_skipped;
  grparse::data_counters().cv_enrichment_skipped.fetch_add(1, std::memory_order_relaxed);
  require(grparse::data_totals().cv_enrichment_skipped == skipped + 1,
          "a recorded enrichment skip does not reach the exported totals");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("data-contract-origin-test", "ok", {
      verify_the_mimetype_ladder_is_stamped_on_the_origin,
      verify_the_evidence_rides_as_a_typed_claim,
      verify_a_metadata_title_becomes_one_attributed_title_item,
      verify_the_disabled_enrichment_leg_is_a_total_no_op,
  });
}
