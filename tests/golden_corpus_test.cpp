// The golden corpus, read directly: tests/golden/corpus against its typed
// manifest (tests/golden/corpus_manifest.textproto). Every InputFormat value
// the parse_types descriptor lists must have at least one fixture; every
// fixture must resolve, through the same sniff ladder and InputFormat
// mapping a request takes, to the format the manifest states, and route to
// the collector it states; the manifest and the directory must list the same
// files. The full golden of most fixtures needs a collector, the PDF backend
// or the model files, and the manifest says which tier can run it; that claim
// is checked against the route here, so a tier cannot be stated that the
// routing contradicts. Regenerate the formats/ fixtures with
// tests/golden/build_format_fixtures.py.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <print>
#include <set>
#include <string>
#include <vector>

#include "ai/pipestream/parse/v1/parse_types.pb.h"
#include "corpus_manifest.pb.h"
#include "google/protobuf/text_format.h"
#include "grparse/collector_coordinator.h"
#include "grparse/content_sniff.h"
#include "grparse/input_format.h"
#include "support/check.h"

namespace fs = std::filesystem;
namespace golden = grparse::test::golden;
namespace parsev1 = ai::pipestream::parse::v1;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

// The issue's ceiling for the whole corpus, formats/ included (#257). The
// scorecard's own, tighter ceiling covers the top-level files it scores.
constexpr std::uintmax_t kCorpusCeilingBytes = 4u * 1024u * 1024u;

std::string env_or(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' ? std::string(value) : fallback;
}

std::string source_root() { return env_or("GRPARSE_SOURCE_ROOT", "."); }

fs::path corpus_dir() {
  return env_or("GRPARSE_TEST_CORPUS_DIR", source_root() + "/tests/golden/corpus");
}

fs::path manifest_path() {
  return env_or("GRPARSE_GOLDEN_MANIFEST",
                source_root() + "/tests/golden/corpus_manifest.textproto");
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  require(in.good(), "open " + path.string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

const golden::CorpusManifest& manifest() {
  static const golden::CorpusManifest loaded = [] {
    golden::CorpusManifest parsed;
    require(google::protobuf::TextFormat::ParseFromString(read_file(manifest_path()), &parsed),
            "parse " + manifest_path().string() + " as grparse.test.golden.CorpusManifest");
    return parsed;
  }();
  return loaded;
}

// The InputFormat values (UNSPECIFIED aside) no fixture of `of` states, by
// enum name, in descriptor order.
std::vector<std::string> formats_without_fixture(const golden::CorpusManifest& of) {
  std::set<int> covered;
  for (const golden::CorpusFixture& fixture : of.fixtures()) covered.insert(fixture.format());
  std::vector<std::string> missing;
  const auto* formats = parsev1::InputFormat_descriptor();
  for (int i = 0; i < formats->value_count(); ++i) {
    const auto* value = formats->value(i);
    if (value->number() == parsev1::INPUT_FORMAT_UNSPECIFIED) continue;
    if (!covered.contains(value->number())) missing.emplace_back(std::string(value->name()));
  }
  return missing;
}

std::string joined(const std::vector<std::string>& names) {
  std::string out;
  for (const std::string& name : names) out += (out.empty() ? "" : ", ") + name;
  return out;
}

// Where a full golden of the fixture can run, from what parses it: the wiki
// storage handler is in process; the CV path is in process with models for
// a raster, needs the PDF backend service for a PDF, and decodes nothing
// else; every other collector is a compose-stack service.
golden::GoldenTier tier_the_route_allows(const golden::CorpusFixture& fixture) {
  if (fixture.header_only()) return golden::GOLDEN_TIER_NONE;
  switch (fixture.collector()) {
    case parsev1::COLLECTOR_CONFLUENCE:
      return golden::GOLDEN_TIER_UNIT;
    case parsev1::COLLECTOR_GRPARSE_CV:
      if (fixture.format() == parsev1::INPUT_FORMAT_IMAGE) return golden::GOLDEN_TIER_MODELS;
      if (fixture.format() == parsev1::INPUT_FORMAT_PDF) return golden::GOLDEN_TIER_STACK;
      return golden::GOLDEN_TIER_NONE;
    default:
      return golden::GOLDEN_TIER_STACK;
  }
}

void verify_every_input_format_has_a_fixture() {
  const std::vector<std::string> missing = formats_without_fixture(manifest());
  require(missing.empty(),
          "InputFormat values with no fixture in tests/golden/corpus (add one with "
          "tests/golden/build_format_fixtures.py and list it in corpus_manifest.textproto): " +
              joined(missing));
}

// The coverage check itself, against the real manifest with each format's
// fixtures taken out in turn: it must name exactly that format, so a new
// enum value without a fixture, or a deleted fixture, cannot pass.
void verify_the_coverage_check_names_each_removed_format() {
  const auto* formats = parsev1::InputFormat_descriptor();
  for (int i = 0; i < formats->value_count(); ++i) {
    const auto* value = formats->value(i);
    if (value->number() == parsev1::INPUT_FORMAT_UNSPECIFIED) continue;
    golden::CorpusManifest without;
    for (const golden::CorpusFixture& fixture : manifest().fixtures()) {
      if (fixture.format() != value->number()) *without.add_fixtures() = fixture;
    }
    const std::string name(value->name());
    require_equal(joined(formats_without_fixture(without)), name,
                  "the coverage check with every " + name + " fixture removed");
  }
}

void verify_the_manifest_and_the_directory_agree() {
  std::set<std::string> listed;
  for (const golden::CorpusFixture& fixture : manifest().fixtures()) {
    const fs::path path(fixture.path());
    require(!fixture.path().empty() && path.is_relative() &&
                fixture.path().find("..") == std::string::npos,
            "a fixture path is relative to the corpus and stays inside it: '" +
                fixture.path() + "'");
    require(listed.insert(path.generic_string()).second,
            "fixture listed twice: " + fixture.path());
    require(!fixture.note().empty(), fixture.path() + " says what it holds (note)");
  }
  std::set<std::string> present;
  std::uintmax_t total = 0;
  const fs::path root = corpus_dir();
  require(fs::is_directory(root), "corpus directory " + root.string());
  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    present.insert(fs::relative(entry.path(), root).generic_string());
    total += entry.file_size();
  }
  std::vector<std::string> unlisted;
  std::vector<std::string> absent;
  for (const std::string& name : present) {
    if (!listed.contains(name)) unlisted.push_back(name);
  }
  for (const std::string& name : listed) {
    if (!present.contains(name)) absent.push_back(name);
  }
  require(unlisted.empty(), "corpus files corpus_manifest.textproto does not list: " +
                                joined(unlisted));
  require(absent.empty(), "corpus_manifest.textproto lists files the corpus does not hold: " +
                              joined(absent));
  require(total <= kCorpusCeilingBytes,
          "the corpus holds " + std::to_string(total) + " bytes, above its " +
              std::to_string(kCorpusCeilingBytes) + "-byte ceiling; shrink a fixture");
}

void verify_each_fixture_resolves_and_routes_as_stated() {
  std::map<std::string, int> by_tier;
  for (const golden::CorpusFixture& fixture : manifest().fixtures()) {
    const std::string bytes = read_file(corpus_dir() / fixture.path());
    require(!bytes.empty(), fixture.path() + " is not empty");
    // The name a request carries is the bare file name, not the corpus path.
    const std::string name = fs::path(fixture.path()).filename().string();
    const grparse::MimetypeResolution resolved = grparse::resolve_mimetype("", bytes, name);
    const auto format = grparse::input_format_for(resolved.mimetype, name);
    const std::string where =
        fixture.path() + " (" + resolved.mimetype + " by " + resolved.evidence + ")";
    if (fixture.declared_only()) {
      require(fixture.format() != parsev1::INPUT_FORMAT_UNSPECIFIED,
              fixture.path() + ": a declared_only fixture states the format a caller declares");
    }
    if (fixture.declared_only() || fixture.format() == parsev1::INPUT_FORMAT_UNSPECIFIED) {
      require(!format.has_value(),
              where + " must resolve to no InputFormat, but resolves to " +
                  (format ? parsev1::InputFormat_Name(*format) : std::string()));
    } else {
      require(format.has_value(), where + " resolves to no InputFormat; the manifest says " +
                                      parsev1::InputFormat_Name(fixture.format()));
      require_equal(parsev1::InputFormat_Name(*format), parsev1::InputFormat_Name(fixture.format()),
                    where + " resolves to the InputFormat the manifest states");
    }
    // A declared_only fixture's collector is the caller's selection; every
    // other fixture reaches its collector by the default route.
    if (!fixture.declared_only()) {
      require_equal(parsev1::Collector_Name(grparse::route_document(name, "", bytes)),
                    parsev1::Collector_Name(fixture.collector()),
                    where + " routes to the collector the manifest states");
    }
    require_equal(golden::GoldenTier_Name(fixture.full_golden()),
                  golden::GoldenTier_Name(tier_the_route_allows(fixture)),
                  fixture.path() + ": the full-golden tier is the one its route allows");
    ++by_tier[golden::GoldenTier_Name(fixture.full_golden())];
  }
  std::string summary;
  for (const auto& [tier, count] : by_tier) {
    summary += (summary.empty() ? "" : ", ") + tier + "=" + std::to_string(count);
  }
  std::println("golden-corpus-test: {} fixtures sniffed and routed; full golden tiers: {}",
               manifest().fixtures_size(), summary);
}

}  // namespace

int main() {
  return grparse_test::run_test_main("golden-corpus-test", "ok", {
      verify_the_manifest_and_the_directory_agree,
      verify_every_input_format_has_a_fixture,
      verify_the_coverage_check_names_each_removed_format,
      verify_each_fixture_resolves_and_routes_as_stated,
  });
}
