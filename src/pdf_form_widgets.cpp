#include "grparse/pdf_form_widgets.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <numeric>
#include <string_view>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "grparse/consensus_page_source.h"
#include "grparse/document_assembly.h"
#include "grparse/document_geometry.h"
#include "grparse/pdf_page_frame.h"
#include "grparse/remote_page_source.h"
#include "targets/sha256.h"

namespace grparse {
namespace {

namespace docv1 = ai::pipestream::document::v1;
namespace pdfv1 = ai::protomolt::parse::pdf::v1;

constexpr int kMaxMessageBytes = 64 * 1024 * 1024;
// One family over every page: no rasters, no text, so the budget is the
// page walk alone.
constexpr auto kWidgetDeadline = std::chrono::seconds(120);
constexpr std::string_view kPointsUnit = "pt";
// ISO 32000-1 field flag bit 1 (table 221).
constexpr uint32_t kFieldFlagReadOnly = 1U;

docv1::FormFieldKind document_kind(pdfv1::FormFieldKind kind) {
  switch (kind) {
    case pdfv1::FORM_FIELD_KIND_TEXT:
      return docv1::FORM_FIELD_KIND_TEXT;
    case pdfv1::FORM_FIELD_KIND_CHECK_BOX:
      return docv1::FORM_FIELD_KIND_CHECK_BOX;
    case pdfv1::FORM_FIELD_KIND_RADIO_BUTTON:
      return docv1::FORM_FIELD_KIND_RADIO_BUTTON;
    case pdfv1::FORM_FIELD_KIND_PUSH_BUTTON:
      return docv1::FORM_FIELD_KIND_PUSH_BUTTON;
    case pdfv1::FORM_FIELD_KIND_COMBO_BOX:
      return docv1::FORM_FIELD_KIND_COMBO_BOX;
    case pdfv1::FORM_FIELD_KIND_LIST_BOX:
      return docv1::FORM_FIELD_KIND_LIST_BOX;
    case pdfv1::FORM_FIELD_KIND_SIGNATURE:
      return docv1::FORM_FIELD_KIND_SIGNATURE;
    default:
      return docv1::FORM_FIELD_KIND_UNSPECIFIED;
  }
}

bool is_button(pdfv1::FormFieldKind kind) {
  return kind == pdfv1::FORM_FIELD_KIND_CHECK_BOX ||
         kind == pdfv1::FORM_FIELD_KIND_RADIO_BUTTON;
}

// A button's value is a PDF name; the document keeps the bare state name
// whichever way a backend spelled it.
std::string field_value(const pdfv1::FormField& field) {
  std::string value = field.value();
  if (is_button(field.kind()) && value.starts_with('/')) value.erase(0, 1);
  return value;
}

// A check box or radio button shows its on state when the widget's
// appearance state names anything but /Off; a widget without /AS falls back
// to the field value.
bool button_selected(const pdfv1::FormField& field) {
  if (field.has_appearance_state()) {
    return !field.appearance_state().empty() && field.appearance_state() != "/Off";
  }
  const std::string value = field_value(field);
  return !value.empty() && value != "Off";
}

// How one page's contract boxes map onto the page space the document
// already uses: the frame places them on the rendered page (top-left
// points), and the scale and origin take them from there.
struct PageSpace {
  explicit PageSpace(const pdfv1::PageInfo& info) : frame(info) {}
  PdfPageFrame frame;
  double scale_x = 1.0;
  double scale_y = 1.0;
  docv1::CoordOrigin origin = docv1::COORD_ORIGIN_BOTTOMLEFT;
};

// The origin the boxes already on `page` state, when any does.
std::optional<docv1::CoordOrigin> page_origin(const docv1::Document& document, int page) {
  auto first = [page](const auto& prov) -> std::optional<docv1::CoordOrigin> {
    for (const auto& entry : prov) {
      if (entry.page_no() == page && entry.has_bbox() && entry.bbox().has_coord_origin() &&
          entry.bbox().coord_origin() != docv1::COORD_ORIGIN_UNSPECIFIED) {
        return entry.bbox().coord_origin();
      }
    }
    return std::nullopt;
  };
  for (const auto& item : document.texts()) {
    std::optional<docv1::CoordOrigin> found;
    if (item.item_case() == docv1::BaseTextItem::kCode) {
      found = first(item.code().prov());
    } else if (const auto* base = text_base_of(item); base != nullptr) {
      found = first(base->prov());
    }
    if (found.has_value()) return found;
  }
  for (const auto& table : document.tables()) {
    if (auto found = first(table.prov())) return found;
  }
  for (const auto& picture : document.pictures()) {
    if (auto found = first(picture.prov())) return found;
  }
  return std::nullopt;
}

PageSpace page_space(const docv1::Document& document, const PdfPageWidgets& page) {
  PageSpace space(page.page_info);
  const double width_pts = space.frame.display_width();
  const double height_pts = space.frame.display_height();
  const auto found = document.pages().find(page.page_number);
  const bool sized = found != document.pages().end() &&
                     found->second.size().width() > 0 && found->second.size().height() > 0;
  const bool points = found != document.pages().end() && found->second.has_unit() &&
                      found->second.unit() == kPointsUnit;
  if (sized && !points && width_pts > 0 && height_pts > 0) {
    space.scale_x = found->second.size().width() / width_pts;
    space.scale_y = found->second.size().height() / height_pts;
    space.origin = docv1::COORD_ORIGIN_TOPLEFT;
  }
  if (const auto origin = page_origin(document, page.page_number)) space.origin = *origin;
  // A top-left box needs the page height to flip; without one the points
  // stay bottom-left, which needs none.
  if (height_pts <= 0) space.origin = docv1::COORD_ORIGIN_BOTTOMLEFT;
  return space;
}

docv1::BoundingBox page_box(const pdfv1::BoundingBox& rect, const PageSpace& space) {
  docv1::BoundingBox box;
  const auto [left, top, right, bottom] = space.frame.place(rect);
  box.set_l(left * space.scale_x);
  box.set_r(right * space.scale_x);
  if (space.origin == docv1::COORD_ORIGIN_TOPLEFT) {
    box.set_t(top * space.scale_y);
    box.set_b(bottom * space.scale_y);
  } else {
    const double height = space.frame.display_height();
    box.set_t((height - top) * space.scale_y);
    box.set_b((height - bottom) * space.scale_y);
  }
  box.set_coord_origin(space.origin);
  return box;
}

// The union of two boxes in the same space and origin.
void grow(docv1::BoundingBox* into, const docv1::BoundingBox& box, bool* any) {
  if (!*any) {
    *into = box;
    *any = true;
    return;
  }
  const bool top_left = box.coord_origin() == docv1::COORD_ORIGIN_TOPLEFT;
  into->set_l(std::min(into->l(), box.l()));
  into->set_r(std::max(into->r(), box.r()));
  into->set_t(top_left ? std::min(into->t(), box.t()) : std::max(into->t(), box.t()));
  into->set_b(top_left ? std::max(into->b(), box.b()) : std::min(into->b(), box.b()));
}

void add_source(const std::string& engine,
                google::protobuf::RepeatedPtrField<docv1::SourceType>* sources) {
  auto* collector = sources->Add()->mutable_collector();
  collector->set_collector("grparse");
  if (!engine.empty()) collector->set_model(engine);
}

void add_prov(int page, const docv1::BoundingBox& box, int32_t chars,
              google::protobuf::RepeatedPtrField<docv1::ProvenanceItem>* prov) {
  auto* entry = prov->Add();
  entry->set_page_no(page);
  *entry->mutable_bbox() = box;
  entry->mutable_charspan()->set_start(0);
  entry->mutable_charspan()->set_end(chars);
}

// Fills the parts every text item of the subtree shares.
docv1::TextItemBase* start_text(docv1::TextItemBase* base, const std::string& self_ref,
                                const std::string& parent, docv1::DocItemLabel label,
                                const std::string& text, const std::string& engine) {
  base->set_self_ref(self_ref);
  base->mutable_parent()->set_ref(parent);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_label(label);
  base->set_text(text);
  base->set_orig(text);
  add_source(engine, base->mutable_source());
  return base;
}

// One widget's FieldItem with its key and value children.
void add_field(const pdfv1::FormField& field, int page, const docv1::BoundingBox& box,
               const std::string& region_ref, const std::string& engine,
               docv1::Document* document) {
  const std::string field_ref = "#/field_items/" + std::to_string(document->field_items_size());
  docv1::FieldItem* item = document->add_field_items();
  item->set_self_ref(field_ref);
  item->mutable_parent()->set_ref(region_ref);
  item->set_content_layer(docv1::CONTENT_LAYER_BODY);
  item->set_label(docv1::DOC_ITEM_LABEL_FIELD_ITEM);
  add_prov(page, box, 0, item->mutable_prov());
  add_source(engine, item->mutable_source());

  // The typed widget state; extensions the canonical JSON leaves out.
  if (!field.name().empty()) item->set_field_name(field.name());
  item->set_field_kind(document_kind(field.kind()));
  const std::string value = field_value(field);
  if (!value.empty()) item->set_value(value);
  if (field.has_default_value()) item->set_default_value(field.default_value());
  if (field.has_flags()) item->set_field_flags(field.flags());
  if (field.has_appearance_state()) item->set_appearance_state(field.appearance_state());
  const bool read_only =
      field.read_only() || (field.has_flags() && (field.flags() & kFieldFlagReadOnly) != 0);
  item->set_read_only(read_only);
  if (field.has_alternate_name()) item->set_description(field.alternate_name());
  for (int index = 0; index < field.options_size(); ++index) {
    item->add_options(field.options(index));
    if (!value.empty() && field.options(index) == value && !item->has_selected_index()) {
      item->set_selected_index(index);
    }
  }

  // The key reads like the label a person sees: the tooltip, else the name.
  const std::string key =
      field.has_alternate_name() && !field.alternate_name().empty() ? field.alternate_name()
                                                                    : field.name();
  if (!key.empty()) {
    const std::string key_ref = "#/texts/" + std::to_string(document->texts_size());
    // The key is placed with its widget like the value: a push button has
    // no value item, so its key is the only text the field puts on the
    // page, and a body item without a page is one no reader can find.
    auto* key_base = start_text(document->add_texts()->mutable_text()->mutable_base(), key_ref,
                                field_ref, docv1::DOC_ITEM_LABEL_FIELD_KEY, key, engine);
    add_prov(page, box, static_cast<int32_t>(utf8_codepoint_count(key)),
             key_base->mutable_prov());
    item->add_children()->set_ref(key_ref);
  }

  const std::string value_ref = "#/texts/" + std::to_string(document->texts_size());
  const auto chars = static_cast<int32_t>(utf8_codepoint_count(value));
  switch (field.kind()) {
    case pdfv1::FORM_FIELD_KIND_CHECK_BOX:
    case pdfv1::FORM_FIELD_KIND_RADIO_BUTTON: {
      // The state name shown (Yes, Off, a radio export value), labelled the
      // way the upstream model labels a check box.
      std::string state = field.has_appearance_state() ? field.appearance_state() : value;
      if (state.starts_with('/')) state.erase(0, 1);
      auto* base = start_text(document->add_texts()->mutable_text()->mutable_base(), value_ref,
                              field_ref,
                              button_selected(field) ? docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED
                                                     : docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED,
                              state, engine);
      add_prov(page, box, static_cast<int32_t>(utf8_codepoint_count(state)),
               base->mutable_prov());
      item->add_children()->set_ref(value_ref);
      break;
    }
    case pdfv1::FORM_FIELD_KIND_PUSH_BUTTON:
    case pdfv1::FORM_FIELD_KIND_SIGNATURE:
      // A push button holds no value; a signature's value is a signature
      // dictionary, reported through the backend's signature family.
      break;
    default: {
      // Text and choice fields. Upstream's two value kinds: a slot the
      // reader may fill, or a value the form fixes.
      auto* value_item = document->add_texts()->mutable_field_value();
      auto* base = start_text(value_item->mutable_base(), value_ref, field_ref,
                              docv1::DOC_ITEM_LABEL_FIELD_VALUE, value, engine);
      value_item->set_kind(read_only ? "read_only" : "fillable");
      add_prov(page, box, chars, base->mutable_prov());
      item->add_children()->set_ref(value_ref);
      break;
    }
  }
}

// The running page of each body child: its own first page when it has a
// usable box, else the page of the child before it (an unplaced item rides
// with its predecessor, as in the body reading order).
std::vector<int> running_pages(const docv1::Document& document) {
  const std::map<int, double> heights = document_page_heights(document);
  std::vector<int> pages;
  pages.reserve(static_cast<size_t>(document.body().children_size()));
  int current = 0;
  for (const auto& child : document.body().children()) {
    if (const auto placement = item_placement(document, child.ref(), heights)) {
      current = placement->page;
    }
    pages.push_back(current);
  }
  return pages;
}

}  // namespace

void fold_pdf_form_widgets(const std::vector<PdfPageWidgets>& pages, const std::string& engine,
                           docv1::Document* document) {
  if (document == nullptr) return;
  std::vector<const PdfPageWidgets*> ordered;
  for (const auto& page : pages) {
    if (page.page_number > 0 && !page.fields.empty()) ordered.push_back(&page);
  }
  if (ordered.empty()) return;
  std::stable_sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
    return a->page_number < b->page_number;
  });

  // Where each page's region enters the body, from the body as it stands:
  // after the last child whose running page is this page or an earlier one.
  // The positions never decrease with the page, so regions keep page order.
  const std::vector<int> running = running_pages(*document);
  std::vector<std::pair<size_t, std::string>> inserts;

  for (const PdfPageWidgets* page : ordered) {
    const PageSpace space = page_space(*document, *page);
    // Reading order on the page: top edge first, then left edge; ties keep
    // the backend's /Annots order.
    std::vector<size_t> order(page->fields.size());
    std::iota(order.begin(), order.end(), size_t{0});
    // Measured on the rendered page, so a turned page reads as shown.
    std::vector<std::array<double, 4>> placed;
    placed.reserve(page->fields.size());
    for (const auto& field : page->fields) placed.push_back(space.frame.place(field.rect()));
    std::stable_sort(order.begin(), order.end(), [&placed](size_t a, size_t b) {
      if (placed[a][1] != placed[b][1]) return placed[a][1] < placed[b][1];
      return placed[a][0] < placed[b][0];
    });

    const std::string region_ref =
        "#/field_regions/" + std::to_string(document->field_regions_size());
    docv1::FieldRegionItem* region = document->add_field_regions();
    region->set_self_ref(region_ref);
    region->mutable_parent()->set_ref("#/body");
    region->set_content_layer(docv1::CONTENT_LAYER_BODY);
    region->set_label(docv1::DOC_ITEM_LABEL_FIELD_REGION);
    add_source(engine, region->mutable_source());

    docv1::BoundingBox extent;
    bool any = false;
    for (const size_t index : order) {
      const docv1::BoundingBox box = page_box(page->fields[index].rect(), space);
      grow(&extent, box, &any);
      const std::string field_ref =
          "#/field_items/" + std::to_string(document->field_items_size());
      add_field(page->fields[index], page->page_number, box, region_ref, engine, document);
      // add_field may grow the field arena; re-fetch the region.
      document->mutable_field_regions(document->field_regions_size() - 1)
          ->add_children()
          ->set_ref(field_ref);
    }
    add_prov(page->page_number, extent, 0,
             document->mutable_field_regions(document->field_regions_size() - 1)->mutable_prov());

    size_t position = 0;
    for (size_t index = 0; index < running.size(); ++index) {
      if (running[index] <= page->page_number) position = index + 1;
    }
    inserts.emplace_back(position, region_ref);
  }

  // Rebuild the body's child list with the regions spliced in.
  google::protobuf::RepeatedPtrField<docv1::RefItem> children;
  const auto& existing = document->body().children();
  size_t next = 0;
  for (int index = 0; index <= existing.size(); ++index) {
    while (next < inserts.size() && inserts[next].first == static_cast<size_t>(index)) {
      children.Add()->set_ref(inserts[next].second);
      ++next;
    }
    if (index < existing.size()) *children.Add() = existing.Get(index);
  }
  *document->mutable_body()->mutable_children() = std::move(children);
}

PdfWidgetFetch fetch_pdf_form_widgets(const std::string& bytes, const std::string& target,
                                      std::chrono::system_clock::time_point deadline) {
  PdfWidgetFetch fetch;
  try {
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(kMaxMessageBytes);
    args.SetMaxSendMessageSize(kMaxMessageBytes);
    auto stub = pdfv1::PdfBackendService::NewStub(
        grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(), args));
    const char* handshake_env = std::getenv("GRPARSE_PDF_BACKEND_HANDSHAKE");
    const bool handshake =
        handshake_env == nullptr || std::string_view(handshake_env) != "off";
    const std::string sha256 = handshake ? targets::sha256_hex(bytes) : std::string();
    const auto budget = std::chrono::system_clock::now() + kWidgetDeadline;
    bool sent_bytes = !handshake;
    for (;;) {
      grpc::ClientContext context;
      context.set_deadline(std::min(deadline, budget));
      pdfv1::ParseRequest request;
      if (handshake) request.mutable_document()->set_sha256(sha256);
      if (sent_bytes) request.mutable_document()->set_data(bytes);
      request.add_families(pdfv1::PDF_FAMILY_FORM_FIELDS);
      auto reader = stub->Parse(&context, request);

      pdfv1::ParseResponse message;
      std::map<uint32_t, pdfv1::PageInfo> infos;
      std::map<uint32_t, std::vector<pdfv1::FormField>> fields;
      std::optional<pdfv1::LoadStatus> load_status;
      std::string load_detail;
      bool skipped = false;
      while (reader->Read(&message)) {
        if (message.has_header()) {
          const auto& caps = message.header().capabilities();
          load_status = caps.load_status();
          load_detail = caps.load_detail();
          fetch.engine = caps.backend_name();
          for (const auto& info : message.header().pages()) {
            infos[info.page_index()] = info;
          }
          if (caps.load_status() != pdfv1::LOAD_STATUS_OK) continue;
          // The contract reads a family missing from the verdicts as
          // unsupported. Absent or unsupported, there is nothing to wait
          // for, and the page walk is cancelled rather than drained.
          auto support = pdfv1::FAMILY_SUPPORT_UNSUPPORTED_BY_BACKEND;
          for (const auto& verdict : caps.families()) {
            if (verdict.family() == pdfv1::PDF_FAMILY_FORM_FIELDS) support = verdict.support();
          }
          if (support != pdfv1::FAMILY_SUPPORT_SUPPORTED) {
            fetch.ok = true;
            fetch.unsupported = support != pdfv1::FAMILY_SUPPORT_ABSENT_IN_DOCUMENT;
            skipped = true;
            context.TryCancel();
            break;
          }
        } else if (message.has_page()) {
          for (const auto& field : message.page().form_fields()) {
            fields[message.page().page_index()].push_back(field);
          }
        }
      }
      const grpc::Status status = reader->Finish();
      if (skipped) return fetch;
      if (!status.ok()) {
        fetch.error = target + ": " + status.error_message();
        return fetch;
      }
      if (load_status == pdfv1::LOAD_STATUS_BYTES_REQUIRED && !sent_bytes) {
        sent_bytes = true;
        continue;
      }
      if (load_status != pdfv1::LOAD_STATUS_OK) {
        fetch.error = target + ": " +
                      pdfv1::LoadStatus_Name(load_status.value_or(pdfv1::LOAD_STATUS_UNSPECIFIED)) +
                      (load_detail.empty() ? "" : " (" + load_detail + ")");
        return fetch;
      }
      for (auto& [index, list] : fields) {
        PdfPageWidgets page;
        page.page_number = static_cast<int>(index) + 1;
        if (const auto info = infos.find(index); info != infos.end()) {
          page.page_info = info->second;
        }
        page.fields = std::move(list);
        fetch.pages.push_back(std::move(page));
      }
      fetch.ok = true;
      return fetch;
    }
  } catch (const std::exception& error) {
    fetch.ok = false;
    fetch.error = target + ": " + error.what();
    return fetch;
  }
}

std::optional<std::string> fold_pdf_form_widgets_from_backend(
    const std::string& bytes, std::chrono::system_clock::time_point deadline,
    docv1::Document* document) {
  std::optional<std::string> configured;
  try {
    configured = remote_pdf_backend_target();
  } catch (const std::exception& error) {
    return std::string("AcroForm widgets not read: ") + error.what();
  }
  if (!configured.has_value() || document == nullptr) return std::nullopt;
  std::vector<std::string> failures;
  for (const std::string& target : split_backend_targets(*configured)) {
    PdfWidgetFetch fetch = fetch_pdf_form_widgets(bytes, target, deadline);
    if (!fetch.ok) {
      failures.push_back(std::move(fetch.error));
      continue;
    }
    // A backend without the family is no answer; the next one may have it.
    if (fetch.unsupported) continue;
    fold_pdf_form_widgets(fetch.pages, fetch.engine, document);
    return std::nullopt;
  }
  if (failures.empty()) return std::nullopt;
  std::string warning = "AcroForm widgets not read:";
  for (const std::string& failure : failures) warning += " " + failure + ";";
  warning.pop_back();
  return warning;
}

}  // namespace grparse
