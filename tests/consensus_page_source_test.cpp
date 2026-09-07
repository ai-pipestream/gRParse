// Exercises the consensus page source against in-process fake backends
// with differing word orders: the bigram vote must return the majority
// order no matter where the scrambled backend is in the target list, a
// backend that cannot load the document must drop out of the vote, and
// target splitting must trim.
#include <memory>
#include <print>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include <set>

#include "ai/pipestream/document/v1/document.pb.h"
#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "grparse/consensus_page_source.h"
#include "grparse/document_assembly.h"
#include "grparse/in_memory_document.h"
#include "support/check.h"

namespace {

using grparse_test::require;
namespace pdfv1 = ai::protomolt::parse::pdf::v1;

constexpr double kPageWidthPts = 612.0;
constexpr double kPageHeightPts = 792.0;

// One page per entry, cells the words handed to the constructor, one word
// per cell, laid out left to right with uniform line-height boxes. Parse
// fails chosen pages with an RPC error, Render fails wholesale, and both
// count their calls so the tests can assert exactly what was dialed.
class FakeBackend final : public pdfv1::PdfBackendService::Service {
 public:
  FakeBackend(std::string name, std::vector<std::vector<std::string>> pages,
              bool loads)
      : name_(std::move(name)), pages_(std::move(pages)), loads_(loads) {}

  // Zero-based pages whose Parse answers with an RPC error.
  std::set<uint32_t> parse_fail_pages;
  bool fail_render = false;
  int parse_calls = 0;
  int render_calls = 0;

  grpc::Status Probe(grpc::ServerContext*, const pdfv1::ProbeRequest*,
                     pdfv1::ProbeResponse* response) override {
    auto* caps = response->mutable_capabilities();
    caps->set_backend_name(name_);
    caps->set_engine_version("test");
    if (!loads_) {
      caps->set_load_status(pdfv1::LOAD_STATUS_CORRUPT);
      return grpc::Status::OK;
    }
    caps->set_load_status(pdfv1::LOAD_STATUS_OK);
    caps->set_page_count(static_cast<uint32_t>(pages_.size()));
    return grpc::Status::OK;
  }

  grpc::Status Parse(grpc::ServerContext*, const pdfv1::ParseRequest* request,
                     grpc::ServerWriter<pdfv1::ParseResponse>* writer) override {
    ++parse_calls;
    const uint32_t page_index = request->pages().begin();
    if (parse_fail_pages.contains(page_index)) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "injected parse failure");
    }
    pdfv1::ParseResponse header;
    auto* caps = header.mutable_header()->mutable_capabilities();
    caps->set_load_status(pdfv1::LOAD_STATUS_OK);
    caps->set_page_count(static_cast<uint32_t>(pages_.size()));
    for (uint32_t i = 0; i < pages_.size(); ++i) {
      auto* info = header.mutable_header()->add_pages();
      info->set_page_index(i);
      info->set_width_pts(kPageWidthPts);
      info->set_height_pts(kPageHeightPts);
    }
    writer->Write(header);

    pdfv1::ParseResponse page;
    auto* chunk = page.mutable_page();
    chunk->set_page_index(page_index);
    double x = 72.0;
    for (const auto& word : pages_[page_index]) {
      auto* cell = chunk->add_text_cells();
      cell->set_text(word);
      cell->mutable_bbox()->set_x0(x);
      cell->mutable_bbox()->set_y0(700.0);
      cell->mutable_bbox()->set_x1(x + 40.0);
      cell->mutable_bbox()->set_y1(712.0);
      x += 50.0;
    }
    writer->Write(page);
    return grpc::Status::OK;
  }

  grpc::Status Render(grpc::ServerContext*, const pdfv1::RenderRequest* request,
                      grpc::ServerWriter<pdfv1::RenderResponse>* writer) override {
    ++render_calls;
    if (fail_render) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "injected render failure");
    }
    pdfv1::RenderResponse msg;
    auto* raster = msg.mutable_raster();
    raster->set_page_index(request->pages().begin());
    raster->set_width_px(10);
    raster->set_height_px(10);
    raster->set_stride_bytes(30);
    raster->set_pixel_format(pdfv1::PIXEL_FORMAT_BGR8);
    raster->set_dpi(request->dpi());
    raster->set_pixels(std::string(300, '\x7f'));
    writer->Write(msg);
    return grpc::Status::OK;
  }

 private:
  std::string name_;
  std::vector<std::vector<std::string>> pages_;
  bool loads_;
};

struct Server {
  std::unique_ptr<FakeBackend> service;
  std::unique_ptr<grpc::Server> server;
  std::string target;
};

Server start(std::string name, std::vector<std::vector<std::string>> pages,
             bool loads) {
  Server out;
  out.service = std::make_unique<FakeBackend>(std::move(name),
                                              std::move(pages), loads);
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(out.service.get());
  out.server = builder.BuildAndStart();
  require(out.server != nullptr && port != 0, "fake backend started");
  out.target = "127.0.0.1:" + std::to_string(port);
  return out;
}

std::string joined_text(const grparse::OcrPage& page) {
  std::string text;
  for (const auto& line : page.lines) {
    if (!text.empty()) text += " ";
    text += line.text;
  }
  return text;
}

}  // namespace

int main() {
  const std::vector<std::string> reading = {
      "The", "survey", "team", "returned", "the", "equipment",
      "on", "time", "and", "in", "good", "condition."};
  std::vector<std::string> scrambled = reading;
  for (size_t i = 0; i + 1 < scrambled.size(); i += 2) {
    std::swap(scrambled[i], scrambled[i + 1]);
  }

  Server good_a = start("good-a", {reading}, true);
  Server good_b = start("good-b", {reading}, true);
  Server bad = start("scrambled", {scrambled}, true);
  Server broken = start("broken", {}, false);

  const auto bytes = std::make_shared<const std::string>("%PDF-fake");
  const std::string expected = "The survey team returned the equipment on time and in good condition.";

  // The majority order wins with the scrambled backend in front, and the
  // reconciliation for the losing streams rides on the page and onto the
  // wire, offsets in both directions.
  {
    const auto source = grparse::open_consensus_pdf_document(
        bytes, {bad.target, good_a.target, good_b.target}, 144.0);
    require(source->page_count() == 1, "page count from the first backend");
    const auto page = source->extract_digital_page(1);
    require(page.has_value(), "consensus page extracted");
    require(joined_text(*page) == expected,
            "majority order wins over the scrambled leader");

    require(page->reconciliation.size() == 2,
            "each losing backend contributes a reconciliation");
    const grparse::SourceReconciliation* scrambled_rec = nullptr;
    const grparse::SourceReconciliation* agreeing_rec = nullptr;
    for (const auto& rec : page->reconciliation) {
      (rec.source == bad.target ? scrambled_rec : agreeing_rec) = &rec;
    }
    require(scrambled_rec != nullptr && agreeing_rec != nullptr,
            "reconciliations name their sources");
    require(scrambled_rec->matched == 12 && scrambled_rec->missing == 0,
            "all words of the scrambled stream link up");
    require(scrambled_rec->order_breaks > 0,
            "the scrambled stream's order breaks are annotated");
    require(agreeing_rec->order_breaks == 0 && agreeing_rec->matched == 12,
            "the agreeing stream links without breaks");
    require(!page->source_order_trusted,
            "a leg full of order breaks keeps the geometric re-order");
    const auto& first = scrambled_rec->links.front();
    require(first.consensus_index == 0 && first.consensus_utf_start == 0,
            "consensus side of the first link");
    // Pairwise swap put "The" second in the scrambled stream, after
    // "survey" and one joining space.
    require(first.source_index.has_value() && *first.source_index == 1 &&
                first.source_utf_start == 7,
            "source side of the first link points into the other stream");

    // The vote's winning half rides the page too: the winner's engine name
    // (the Probe backend name, not the dialed target), its score, and every
    // leg that voted.
    require(page->vote.has_value(), "the vote's winner is recorded");
    require(page->vote->winner.target == good_a.target &&
                page->vote->winner.engine == "good-a",
            "the first agreeing backend wins the tie and names its engine");
    require(page->vote->winner_score > 0.0, "the winning score is recorded");
    require(page->vote->legs.size() == 3, "every voting backend is a leg");

    grparse::AssemblyCursor cursor;
    ai::pipestream::parse::v1::PageData wire;
    grparse::append_page_data(*page, 1, &cursor, &wire);
    require(wire.reconciliation_size() == 2,
            "reconciliation crosses onto the wire");
    bool wire_checked = false;
    for (const auto& rec : wire.reconciliation()) {
      if (rec.source() != bad.target) continue;
      require(rec.matched() == 12 && rec.order_breaks() > 0 &&
                  rec.links_size() == 12,
              "wire counts and links match the page");
      require(rec.links(0).source_index() == 1 &&
                  rec.links(0).source_utf_start() == 7,
              "wire link keeps both offsets");
      wire_checked = true;
    }
    require(wire_checked, "the scrambled source is on the wire");

    // The assembled document claims the vote: one protomolt claim with the
    // winner's engine and score. The fake pages emit one cell per word, so
    // the page is one item per word; the pairwise swap breaks order at
    // every odd word, and exactly those items name the scrambled leg as
    // the source that deviated inside them.
    ai::pipestream::document::v1::Document document;
    std::string plain_text;
    grparse::AssemblyCursor document_cursor;
    grparse::append_page_to_document(*page, 1, &document_cursor, &document,
                                     &plain_text);
    grparse::append_consensus_claim({&*page}, &document);
    require(document.claims_size() == 1, "one claim for the vote");
    const auto& claim = document.claims(0).source();
    require(claim.collector() == "protomolt", "the vote claims as protomolt");
    require(claim.model() == "good-a", "the claim names the winning engine");
    require(claim.raw_score() == page->vote->winner_score &&
                claim.raw_score_kind() == "consensus_bigram_agreement" &&
                claim.raw_score_samples() == 1,
            "the claim carries the winning score, its kind, and its extent");
    require(document.texts_size() == 12, "one item per word cell");
    require(document.texts(0).text().base().field_sources().empty(),
            "an item whose word linked cleanly carries no vote attribution");
    const auto& field_sources =
        document.texts(1).text().base().field_sources();
    require(field_sources.size() == 2,
            "the deviating item names the vote and the deviating loser");
    require(field_sources[0].field() == "text" &&
                field_sources[0].source().collector() == "protomolt" &&
                field_sources[0].source().model() == "good-a",
            "the carried text is the vote winner's reading");
    require(field_sources[1].field() == "text" &&
                field_sources[1].source().collector() == "scrambled",
            "the deviating loser is named on the item");
    bool clean_leg_named = false;
    for (int item = 0; item < document.texts_size(); ++item) {
      for (const auto& entry : document.texts(item).text().base().field_sources()) {
        clean_leg_named = clean_leg_named || entry.source().collector() == "good-b";
      }
    }
    require(!clean_leg_named, "the agreeing leg did not deviate and is absent");
  }

  // A backend that cannot load the document drops out of the vote.
  {
    const auto source = grparse::open_consensus_pdf_document(
        bytes, {broken.target, bad.target, good_a.target, good_b.target},
        144.0);
    const auto page = source->extract_digital_page(1);
    require(page.has_value() && joined_text(*page) == expected,
            "vote proceeds without the backend that failed to load");
    const cv::Mat mat = source->render_page(1);
    require(!mat.empty(), "raster comes from the first loaded backend");
  }

  // Two backends: agreement is symmetric, sentence continuity decides.
  {
    const auto source = grparse::open_consensus_pdf_document(
        bytes, {bad.target, good_a.target}, 144.0);
    const auto page = source->extract_digital_page(1);
    require(page.has_value() && joined_text(*page) == expected,
            "continuity breaks the two-backend tie toward running text");
    require(!page->source_order_trusted,
            "the scrambled leg's break rate is far above the trust ceiling");
  }

  // A clean vote (every leg matched, no order breaks) trusts the winner's
  // emission order and says so on the page.
  {
    const auto source = grparse::open_consensus_pdf_document(
        bytes, {good_a.target, good_b.target}, 144.0);
    const auto page = source->extract_digital_page(1);
    require(page.has_value() && joined_text(*page) == expected,
            "agreeing backends still vote");
    require(page->reconciliation.size() == 1 &&
                page->reconciliation.front().matched == 12 &&
                page->reconciliation.front().order_breaks == 0,
            "the clean leg reconciles completely");
    require(page->source_order_trusted,
            "a clean vote marks the page source-order-trusted");
  }

  // One healthy backend never votes, so nothing is trusted, nothing is
  // recorded, and the assembled document carries no claim at all.
  {
    const auto source = grparse::open_consensus_pdf_document(
        bytes, {good_a.target}, 144.0);
    const auto page = source->extract_digital_page(1);
    require(page.has_value() && !page->source_order_trusted,
            "a single-candidate page is never trusted");
    require(!page->vote.has_value() && page->reconciliation.empty(),
            "a single-candidate page never voted");
    ai::pipestream::document::v1::Document document;
    std::string plain_text;
    grparse::AssemblyCursor cursor;
    grparse::append_page_to_document(*page, 1, &cursor, &document, &plain_text);
    grparse::append_consensus_claim({&*page}, &document);
    require(document.claims_size() == 0, "no vote, no claim");
    bool attributed = false;
    for (const auto& text : document.texts()) {
      attributed = attributed || !text.text().base().field_sources().empty();
    }
    require(!attributed, "no vote, no field sources");
  }

  // --- Multi-page failure paths ----------------------------------------
  // A four-page story, two agreeing replicas per scenario. The pages are
  // short on purpose: every word is one cell, so a page is one line per
  // word and the vote's fold is fully exercised by the assertions.
  const std::vector<std::vector<std::string>> story = {
      {"The", "survey", "team", "returned"},
      {"the", "equipment", "on", "time"},
      {"and", "in", "good", "condition"},
      {"before", "the", "afternoon", "deadline"},
  };
  const std::vector<std::string> story_text = {
      "The survey team returned",
      "the equipment on time",
      "and in good condition",
      "before the afternoon deadline",
  };

  // A backend that passes Probe but fails Parse on one page leaves that
  // page's vote to the healthy legs; the document still completes and the
  // leg is dialed for every page (one failure does not trip anything).
  {
    Server flaky = start("flaky", story, true);
    Server replica_a = start("replica-a", story, true);
    Server replica_b = start("replica-b", story, true);
    flaky.service->parse_fail_pages = {1};  // zero-based: document page 2

    const auto source = grparse::open_consensus_pdf_document(
        bytes, {flaky.target, replica_a.target, replica_b.target}, 144.0);
    require(source->page_count() == 4, "multi-page count from the first backend");
    for (int page_number = 1; page_number <= 4; ++page_number) {
      const auto page = source->extract_digital_page(page_number);
      require(page.has_value() && joined_text(*page) == story_text[page_number - 1],
              "every page completes with the majority reading");
    }
    require(flaky.service->parse_calls == 4,
            "one failed page does not stop the leg being dialed");
    const auto page2 = source->extract_digital_page(2);
    require(page2->vote.has_value() && page2->vote->legs.size() == 2,
            "the failed leg casts no vote on the page it missed");
    require(page2->reconciliation.size() == 1,
            "only the healthy losing leg reconciles on that page");
  }

  // The circuit breaker: three consecutive per-page failures drop the leg
  // for the rest of the document, so page four costs two Parses, not
  // three, and its vote runs without the dead leg.
  {
    Server tripping = start("tripping", story, true);
    Server steady_a = start("steady-a", story, true);
    Server steady_b = start("steady-b", story, true);
    // Fails pages 1-3 (zero-based 0-2); page 4 would serve.
    tripping.service->parse_fail_pages = {0, 1, 2};

    const auto source = grparse::open_consensus_pdf_document(
        bytes, {tripping.target, steady_a.target, steady_b.target}, 144.0);
    for (int page_number = 1; page_number <= 4; ++page_number) {
      const auto page = source->extract_digital_page(page_number);
      require(page.has_value() && joined_text(*page) == story_text[page_number - 1],
              "the vote carries every page while a leg fails");
    }
    require(tripping.service->parse_calls == 3,
            "the tripped leg is never dialed for page four");
    const auto page4 = source->extract_digital_page(4);
    require(page4->vote.has_value() && page4->vote->legs.size() == 2,
            "the disabled leg casts no vote once dropped");
  }

  // A page the leg serves resets the counter: failing pages 1, then
  // serving page 2, then failing 3-5 trips the breaker only after the
  // fifth page, so all five pages were dialed.
  {
    const std::vector<std::vector<std::string>> tale = {
        story[0], story[1], story[2], story[3], {"with", "the", "truck", "idling"},
    };
    Server resetting = start("resetting", tale, true);
    Server stable_a = start("stable-a", tale, true);
    Server stable_b = start("stable-b", tale, true);
    // Fails pages 1, 3, 4, 5 (zero-based 0, 2, 3, 4); page 2 serves.
    resetting.service->parse_fail_pages = {0, 2, 3, 4};

    const auto source = grparse::open_consensus_pdf_document(
        bytes, {resetting.target, stable_a.target, stable_b.target}, 144.0);
    require(source->page_count() == 5, "five-page count from the first backend");
    for (int page_number = 1; page_number <= 4; ++page_number) {
      const auto page = source->extract_digital_page(page_number);
      require(page.has_value(),
              "the healthy legs carry every page while the counter resets");
    }
    const auto page5 = source->extract_digital_page(5);
    require(page5.has_value() && joined_text(*page5) == "with the truck idling",
            "page five still reads through the healthy legs");
    require(resetting.service->parse_calls == 5,
            "the served page reset the counter, so every page was dialed");
  }

  // Render falls through: a backend whose Render fails never serves the
  // raster, and the next target does.
  {
    Server render_bad = start("render-bad", story, true);
    Server render_good = start("render-good", story, true);
    render_bad.service->fail_render = true;

    const auto source = grparse::open_consensus_pdf_document(
        bytes, {render_bad.target, render_good.target}, 144.0);
    const cv::Mat mat = source->render_page(1);
    require(!mat.empty(), "the second backend serves the raster");
    require(render_bad.service->render_calls == 1 &&
                render_good.service->render_calls == 1,
            "render is tried once per target, in order");
  }

  // The vote's word fold: ASCII case, curly quotes and dashes, soft
  // hyphens, Latin ligatures, and Latin-1 uppercase all land on the same
  // folded word the reconciliation keys on.
  {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"PDF", "pdf"},
        {"MixedCASE", "mixedcase"},
        {"don\xE2\x80\x99t", "don't"},              // right single quote
        {"\xE2\x80\x9Chello\xE2\x80\x9D", "\"hello\""},  // double curly quotes
        {"a\xE2\x80\x93" "b", "a-b"},                // en dash
        {"a\xE2\x80\x94" "b", "a-b"},                // em dash
        {"hy\xC2\xADphen", "hyphen"},               // soft hyphen
        {"\xEF\xAC\x81le", "file"},                 // fi ligature
        {"\xEF\xAC\x80\xEF\xAC\x84", "ffffl"},      // ff + ffl ligatures
        {"\xEF\xAC\x83ne", "ffine"},                // ffi ligature
        {"\xEF\xAC\x85op", "stop"},                 // long s t ligature
        {"CAF\xC3\x89", "caf\xC3\xA9"},             // Latin-1 uppercase E acute
    };
    for (const auto& [in, want] : cases) {
      require(grparse::fold_word(in) == want,
              "fold_word(\"" + in + "\") is \"" + want + "\"");
    }
  }

  const auto targets =
      grparse::split_backend_targets(" a:1 , b:2,c:3 ,, ");
  require(targets == std::vector<std::string>({"a:1", "b:2", "c:3"}),
          "target splitting trims and skips empties");

  std::println("consensus-page-source-test: all checks passed");
  return 0;
}
