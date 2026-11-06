// Field-by-field parity between a Docling serve DoclingDocument golden and a
// gRParse Document golden for the same fixture. Both goldens are checked in
// under tests/golden/docling_parity/. Regenerate with
// scripts/regen_docling_parity_goldens.py.
//
// Serve is the reference. Documented deviations live in DEVIATIONS.md and are
// encoded as explicit expectations below, never as loosened assertions.

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "ai/pipestream/document/v1/document.pb.h"
#include "google/protobuf/text_format.h"
#include "support/check.h"

namespace docv1 = ai::pipestream::document::v1;
using grparse_test::require;
using grparse_test::require_equal;

namespace {

std::string repo_root() {
  const char* from_env = std::getenv("GRPARSE_SOURCE_ROOT");
  if (from_env != nullptr && from_env[0] != '\0') return from_env;
  return ".";
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  require(in.good(), "open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

docv1::Document load_document(const std::string& path) {
  const std::string bytes = read_file(path);
  docv1::Document doc;
  if (path.ends_with(".textproto") || path.ends_with(".txtpb")) {
    require(google::protobuf::TextFormat::ParseFromString(bytes, &doc),
            "parse textproto " + path);
  } else {
    require(doc.ParseFromString(bytes), "parse binary " + path);
  }
  return doc;
}

const docv1::TextItemBase* text_base(const docv1::BaseTextItem& item) {
  switch (item.item_case()) {
    case docv1::BaseTextItem::kTitle:
      return &item.title().base();
    case docv1::BaseTextItem::kSectionHeader:
      return &item.section_header().base();
    case docv1::BaseTextItem::kListItem:
      return &item.list_item().base();
    case docv1::BaseTextItem::kFormula:
      return &item.formula().base();
    case docv1::BaseTextItem::kText:
      return &item.text().base();
    default:
      return nullptr;
  }
}

int heading_level(const docv1::BaseTextItem& item) {
  if (item.item_case() == docv1::BaseTextItem::kSectionHeader) {
    return item.section_header().level();
  }
  return 0;
}

constexpr double kBboxTol = 1e-3;

bool near(double a, double b) { return std::fabs(a - b) <= kBboxTol; }

void compare_bbox(const docv1::BoundingBox& a, const docv1::BoundingBox& b,
                  const std::string& where) {
  require_equal(static_cast<int>(a.coord_origin()), static_cast<int>(b.coord_origin()),
                where + " bbox coordinate origin");
  require(near(a.l(), b.l()), where + " bbox.l");
  require(near(a.t(), b.t()), where + " bbox.t");
  require(near(a.r(), b.r()), where + " bbox.r");
  require(near(a.b(), b.b()), where + " bbox.b");
}

// Concatenate plain text in reading order. Docling may fuse multi-page runs
// into one BaseTextItem (prov spanning several pages); gRParse keeps one item
// per page. DEVIATIONS.md documents that texts_size need not match; the
// concatenated reading-order string must.
std::string reading_order_text(const docv1::Document& doc) {
  std::string out;
  std::unordered_map<std::string, const docv1::TextItemBase*> texts;
  std::unordered_map<std::string, const docv1::GroupItem*> groups;
  for (const auto& item : doc.texts()) {
    if (const auto* base = text_base(item)) texts.emplace(base->self_ref(), base);
  }
  for (const auto& group : doc.groups()) groups.emplace(group.self_ref(), &group);
  std::unordered_set<std::string> visited;
  std::function<void(const std::string&)> visit = [&](const std::string& ref) {
    require(visited.insert(ref).second, "duplicate or cyclic body reference " + ref);
    if (const auto it = texts.find(ref); it != texts.end()) {
      if (!out.empty()) out.push_back(' ');
      out.append(it->second->text());
      for (const auto& child : it->second->children()) visit(child.ref());
    } else if (const auto group = groups.find(ref); group != groups.end()) {
      for (const auto& child : group->second->children()) visit(child.ref());
    } else {
      // This text-only fixture must not hide a dangling reference.
      require(false, "unresolved body reference " + ref);
    }
  };
  for (const auto& child : doc.body().children()) visit(child.ref());
  return out;
}

void compare_documents(const docv1::Document& docling,
                       const docv1::Document& grparse,
                       const std::string& fixture) {
  // schema_name: documented deviation; do not require equality.
  require(!grparse.name().empty() || !docling.name().empty(),
          fixture + ": at least one side names the document");

  // Reading-order text must match after a single-space join. texts_size may
  // differ (Docling fused multi-page items vs gRParse per-page items).
  require_equal(reading_order_text(grparse), reading_order_text(docling),
                fixture + " reading_order_text");

  // Heading levels present on both sides must agree when the item counts
  // match; otherwise skip per-item heading compare (covered by DEVIATIONS).
  if (grparse.texts_size() == docling.texts_size()) {
    for (int i = 0; i < docling.texts_size(); ++i) {
      require_equal(static_cast<int>(grparse.texts(i).item_case()),
                    static_cast<int>(docling.texts(i).item_case()),
                    fixture + " texts[" + std::to_string(i) + "] variant");
      require_equal(heading_level(grparse.texts(i)),
                    heading_level(docling.texts(i)),
                    fixture + " texts[" + std::to_string(i) + "] heading level");
      const auto* db = text_base(docling.texts(i));
      const auto* gb = text_base(grparse.texts(i));
      require(db != nullptr && gb != nullptr,
              fixture + " texts[" + std::to_string(i) + "] base");
      require_equal(gb->text(), db->text(),
                    fixture + " texts[" + std::to_string(i) + "] text");
      require_equal(gb->prov_size(), db->prov_size(),
                    fixture + " texts[" + std::to_string(i) + "] prov_size");
      for (int p = 0; p < db->prov_size(); ++p) {
        require_equal(gb->prov(p).page_no(), db->prov(p).page_no(),
                      fixture + " texts[" + std::to_string(i) + "] prov page");
        require_equal(gb->prov(p).has_bbox(), db->prov(p).has_bbox(),
                      fixture + " text bounding box presence");
        if (db->prov(p).has_bbox()) {
          compare_bbox(gb->prov(p).bbox(), db->prov(p).bbox(),
                       fixture + " texts[" + std::to_string(i) + "] prov");
        }
      }
    }
  }

  require_equal(grparse.tables_size(), docling.tables_size(),
                fixture + " tables_size");
  for (int i = 0; i < docling.tables_size(); ++i) {
    const auto& d = docling.tables(i).data();
    const auto& g = grparse.tables(i).data();
    require_equal(g.num_rows(), d.num_rows(),
                  fixture + " tables[" + std::to_string(i) + "] rows");
    require_equal(g.num_cols(), d.num_cols(),
                  fixture + " tables[" + std::to_string(i) + "] cols");
    require_equal(g.table_cells_size(), d.table_cells_size(),
                  fixture + " tables[" + std::to_string(i) + "] cells");
    for (int c = 0; c < d.table_cells_size(); ++c) {
      require_equal(g.table_cells(c).text(), d.table_cells(c).text(),
                    fixture + " tables[" + std::to_string(i) + "] cell text");
      require_equal(g.table_cells(c).start_row_offset_idx(),
                    d.table_cells(c).start_row_offset_idx(),
                    fixture + " tables[" + std::to_string(i) + "] cell row");
      require_equal(g.table_cells(c).start_col_offset_idx(),
                    d.table_cells(c).start_col_offset_idx(),
                    fixture + " tables[" + std::to_string(i) + "] cell col");
      require_equal(g.table_cells(c).row_span(), d.table_cells(c).row_span(),
                    fixture + " tables[" + std::to_string(i) + "] row_span");
      require_equal(g.table_cells(c).col_span(), d.table_cells(c).col_span(),
                    fixture + " tables[" + std::to_string(i) + "] col_span");
    }
  }

  require_equal(grparse.pictures_size(), docling.pictures_size(),
                fixture + " pictures_size");

  require_equal(grparse.pages_size(), docling.pages_size(),
                fixture + " pages_size");
  for (const auto& [page_no, dpage] : docling.pages()) {
    const auto found = grparse.pages().find(page_no);
    require(found != grparse.pages().end(),
            fixture + " missing page " + std::to_string(page_no));
    const auto& gpage = found->second;
    require_equal(gpage.has_size(), dpage.has_size(), fixture + " page size presence");
    if (dpage.has_size()) {
      require(near(gpage.size().width(), dpage.size().width()),
              fixture + " page width");
      require(near(gpage.size().height(), dpage.size().height()),
              fixture + " page height");
    }
    if (dpage.has_quality() && gpage.has_quality()) {
      if (dpage.quality().has_garble_score() &&
          gpage.quality().has_garble_score()) {
        require(near(gpage.quality().garble_score(),
                     dpage.quality().garble_score()),
                fixture + " garble_score");
      }
      // text_layer_replaced: Docling golden will not have it; gRParse may.
    }
  }
}

void verify_hello_text_goldens_match_when_present() {
  const std::string root = repo_root();
  const std::string base = root + "/tests/golden/docling_parity/hello-text";
  const std::string docling_path = base + ".docling.pb";
  const std::string grparse_path = base + ".grparse.pb";
  const docv1::Document docling = load_document(docling_path);
  const docv1::Document grparse = load_document(grparse_path);
  // Explicit deviation pin: Docling fused 2 pages into 1 text item; gRParse
  // emits 1 item per page. See DEVIATIONS.md.
  require_equal(docling.texts_size(), 1, "hello-text docling texts_size");
  require_equal(grparse.texts_size(), 2, "hello-text grparse texts_size");
  compare_documents(docling, grparse, "hello-text");
}

void verify_comparator_rejects_missing_geometry_and_wrong_order() {
  const std::string base = repo_root() + "/tests/golden/docling_parity/hello-text";
  const auto original = load_document(base + ".grparse.pb");
  const auto rejects = [&](docv1::Document altered, const std::string& label) {
    bool rejected = false;
    try { compare_documents(original, altered, "mutation " + label); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "comparator must reject " + label);
  };
  auto altered = original;
  altered.mutable_pages()->at(1).clear_size();
  rejects(altered, "missing page size");
  altered = original;
  altered.mutable_body()->mutable_children()->SwapElements(0, 1);
  rejects(altered, "reversed body reading order");
  altered = original;
  altered.mutable_texts(0)->mutable_text()->mutable_base()->mutable_prov(0)->clear_bbox();
  rejects(altered, "missing text bounding box");
}

void verify_text_layer_replaced_is_optional_extension() {
  docv1::PageQuality q;
  require(!q.has_text_layer_replaced(), "unset by default");
  q.set_text_layer_replaced(true);
  require(q.text_layer_replaced(), "set true");
  std::string bytes;
  require(q.SerializeToString(&bytes), "serialize");
  docv1::PageQuality back;
  require(back.ParseFromString(bytes), "parse");
  require(back.text_layer_replaced(), "round-trip true");
}

}  // namespace

int main() {
  return grparse_test::run_test_main("docling-parity-golden-test", "ok", {
      verify_comparator_rejects_missing_geometry_and_wrong_order,
      verify_text_layer_replaced_is_optional_extension,
      verify_hello_text_goldens_match_when_present,
  });
}
