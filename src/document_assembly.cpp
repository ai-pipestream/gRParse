#include "grparse/document_assembly.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "render/renderer_base.h"
#include "grparse/assembly_blocks.h"
#include "grparse/assembly_table_data.h"
#include "grparse/base64.h"
#include "grparse/reading_order.h"
#include "grparse/region_geometry.h"
#include "grparse/table_structure.h"
#include "grparse/text_geometry.h"

namespace pipestream = ai::pipestream;

namespace grparse {
namespace {

// Attribution when a detector reached assembly without naming itself (test
// doubles and older callers).
const std::string kUnnamedLayoutModel = "layout";

// Every emitted item names the collector and the engine that produced it;
// additive merges with other collectors' output rely on this attribution to
// never collide silently.
void add_collector_source(const std::string& model, std::optional<float> confidence,
                          google::protobuf::RepeatedPtrField<pipestream::document::v1::SourceType>* source) {
  auto* collector = source->Add()->mutable_collector();
  collector->set_collector("grparse");
  collector->set_model(model);
  if (confidence.has_value()) collector->set_confidence(*confidence);
}

pipestream::parse::v1::TextSource text_source_for(const OcrPage& page, const OcrLine& line) {
  if (line.origin.has_value()) {
    return *line.origin == TextOrigin::kDigitalPdf ? pipestream::parse::v1::TEXT_SOURCE_DIGITAL_PDF
                                                   : pipestream::parse::v1::TEXT_SOURCE_OCR;
  }
  switch (page.source) {
    case OcrPage::Source::kDigitalPdf:
      return pipestream::parse::v1::TEXT_SOURCE_DIGITAL_PDF;
    case OcrPage::Source::kMerged:
      // Prefer OCR label only when origin is missing; merged pages should set per-line origin.
      return pipestream::parse::v1::TEXT_SOURCE_OCR;
    case OcrPage::Source::kOcr:
    default:
      return pipestream::parse::v1::TEXT_SOURCE_OCR;
  }
}

// Region label -> document item label for the text lines inside it.  Covers
// both detectors' vocabularies; lines inside table/picture regions keep TEXT,
// because the region itself is emitted as a TableItem/PictureItem whose cells
// come from fill_table_data and whose captions bind once the page is
// assembled (append_page_data). `recognized` false means the label
// is outside both vocabularies and the deliberate structural set: the caller
// keeps the raw spelling on label_raw and names the fallback, because a
// forgotten label must never be invisible.
pipestream::document::v1::DocItemLabel label_for_region(const std::string& label,
                                                        bool* recognized) {
  namespace docv1 = pipestream::document::v1;
  static const std::unordered_map<std::string, docv1::DocItemLabel> kLabels = {
      {"caption", docv1::DOC_ITEM_LABEL_CAPTION},
      {"checkbox_selected", docv1::DOC_ITEM_LABEL_CHECKBOX_SELECTED},
      {"checkbox_unselected", docv1::DOC_ITEM_LABEL_CHECKBOX_UNSELECTED},
      {"code", docv1::DOC_ITEM_LABEL_CODE},
      {"document_index", docv1::DOC_ITEM_LABEL_DOCUMENT_INDEX},
      {"footnote", docv1::DOC_ITEM_LABEL_FOOTNOTE},
      {"form", docv1::DOC_ITEM_LABEL_FORM},
      {"formula", docv1::DOC_ITEM_LABEL_FORMULA},
      {"key_value_region", docv1::DOC_ITEM_LABEL_KEY_VALUE_REGION},
      {"list", docv1::DOC_ITEM_LABEL_LIST_ITEM},
      {"list_item", docv1::DOC_ITEM_LABEL_LIST_ITEM},
      {"page_footer", docv1::DOC_ITEM_LABEL_PAGE_FOOTER},
      {"page_header", docv1::DOC_ITEM_LABEL_PAGE_HEADER},
      {"section_header", docv1::DOC_ITEM_LABEL_SECTION_HEADER},
      {"title", docv1::DOC_ITEM_LABEL_TITLE},
  };
  const auto found = kLabels.find(label);
  if (found != kLabels.end()) {
    *recognized = true;
    return found->second;
  }
  // Structural and empty labels keeping TEXT by design, not by fallback.
  static const std::unordered_set<std::string> kStructuralLabels = {
      "table", "picture", "text", ""};
  if (kStructuralLabels.contains(label)) {
    *recognized = true;
    return docv1::DOC_ITEM_LABEL_TEXT;
  }
  *recognized = false;
  return docv1::DOC_ITEM_LABEL_TEXT;
}

void set_region_bounding_box(const LayoutRegion& region, pipestream::document::v1::BoundingBox* output) {
  output->set_l(region.left);
  output->set_t(region.top);
  output->set_r(region.right);
  output->set_b(region.bottom);
  output->set_coord_origin(pipestream::document::v1::COORD_ORIGIN_TOPLEFT);
}

// True when every vertex of the quad lies on a corner of its own hull, so
// the polygon carries no information the box does not.
bool polygon_is_axis_aligned(const std::vector<cv::Point>& polygon, const AxisAlignedBox& box) {
  for (const auto& vertex : polygon) {
    const bool on_x = vertex.x == box.left || vertex.x == box.right;
    const bool on_y = vertex.y == box.top || vertex.y == box.bottom;
    if (!on_x || !on_y) return false;
  }
  return true;
}

// Big-endian 32-bit read for the PNG IHDR dimensions.
uint32_t read_be32(const unsigned char* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

// Consensus word index -> page line index, over the same word stream the
// vote aligned (whitespace-delimited tokens of page.lines in emission
// order, the split consensus_page_source.cpp votes and reconciles on).
// Empty when the page never voted.
std::vector<uint32_t> consensus_word_lines(const OcrPage& page) {
  std::vector<uint32_t> line_of_word;
  if (!page.vote.has_value()) return line_of_word;
  for (size_t index = 0; index < page.lines.size(); ++index) {
    std::istringstream stream(page.lines[index].text);
    std::string word;
    while (stream >> word) line_of_word.push_back(static_cast<uint32_t>(index));
  }
  return line_of_word;
}

// The engine name a vote leg is claimed under: the name the backend
// reported through Probe, falling back to its dialed target when it never
// named itself.
std::string leg_name(const ConsensusVote::Leg& leg) {
  return leg.engine.empty() ? leg.target : leg.engine;
}

// The losing legs whose reading deviated inside one block's lines: any
// text_deviation or order_break link whose consensus word sits on one of
// those lines. Names are the legs' claim names, in reconciliation order.
std::vector<std::string> deviating_legs(const OcrPage& page,
                                        const std::vector<size_t>& block_lines,
                                        const std::vector<uint32_t>& line_of_word) {
  std::vector<std::string> names;
  for (const auto& rec : page.reconciliation) {
    bool deviates = false;
    for (const auto& link : rec.links) {
      if (deviates) break;
      if (!link.text_deviation && !link.order_break) continue;
      if (link.consensus_index >= line_of_word.size()) continue;
      const uint32_t line = line_of_word[link.consensus_index];
      if (std::ranges::find(block_lines, line) != block_lines.end()) deviates = true;
    }
    if (!deviates) continue;
    std::string name = rec.source;
    for (const auto& leg : page.vote->legs) {
      if (leg.target == rec.source) {
        name = leg_name(leg);
        break;
      }
    }
    names.push_back(std::move(name));
  }
  return names;
}

}  // namespace

// Embed a captured crop as a data URI.  The pixel size comes from the PNG
// IHDR itself, which is authoritative regardless of the page's coordinate
// space (digital pages measure in PDF points, crops in raster pixels).
void set_picture_image(const std::vector<unsigned char>& png,
                       pipestream::document::v1::ImageRef* image) {
  image->set_mimetype("image/png");
  // IHDR starts at byte 8; width and height are its first two fields.
  if (png.size() >= 24) {
    image->mutable_size()->set_width(read_be32(png.data() + 16));
    image->mutable_size()->set_height(read_be32(png.data() + 20));
  }
  image->set_uri("data:image/png;base64," + encode_base64(png.data(), png.size()));
}

uint64_t utf8_codepoint_count(const std::string& text) {
  uint64_t count = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0U) != 0x80U) ++count;
  }
  return count;
}

void append_page_data(const OcrPage& source, int page_number, AssemblyCursor* cursor,
                      pipestream::parse::v1::PageData* output,
                      std::vector<std::string>* warnings) {
  if (cursor == nullptr || output == nullptr) throw std::invalid_argument("Page assembly output is required");
  output->set_page_number(page_number);
  output->mutable_page_meta()->set_page_no(page_number);
  output->mutable_page_meta()->mutable_size()->set_width(source.width);
  output->mutable_page_meta()->mutable_size()->set_height(source.height);
  // The preview's pixel size comes from its own PNG header; the page size
  // above stays in the page's coordinate space (PDF points for digital
  // pages).  Same aspect ratio, so clients can scale boxes onto the image.
  if (!source.preview_png.empty()) {
    set_picture_image(source.preview_png, output->mutable_page_meta()->mutable_image());
  }
  // A turn orientation recovery applied lands in the page's typed quality
  // slot: the page's geometry, size and preview are all in the turned frame,
  // and this is how a client that renders the source itself catches up.
  if (source.rotation_degrees != 0) {
    output->mutable_page_meta()->mutable_quality()->set_rotation_degrees(
        static_cast<double>(source.rotation_degrees));
  }
  // Multi-backend reconciliation (consensus mode only): the page's word
  // links to the other backends' streams ride onto the wire as transport
  // metadata, like text_offsets.
  for (const auto& rec : source.reconciliation) {
    auto* out = output->add_reconciliation();
    out->set_source(rec.source);
    out->set_weight(rec.weight);
    out->set_matched(rec.matched);
    out->set_missing(rec.missing);
    out->set_order_breaks(rec.order_breaks);
    out->set_text_deviations(rec.text_deviations);
    for (const auto& link : rec.links) {
      auto* wire = out->add_links();
      wire->set_consensus_index(link.consensus_index);
      wire->set_consensus_utf_start(link.consensus_utf_start);
      if (link.source_index.has_value()) {
        wire->set_source_index(*link.source_index);
        wire->set_source_utf_start(link.source_utf_start);
      }
      if (link.text_deviation) wire->set_text_deviation(true);
      if (link.order_break) wire->set_order_break(true);
    }
  }

  // Emission order defines text offsets, refs, and body order, so blocks are
  // walked in reading order (multi-column aware, or the trusted source order
  // of a consensus-voted page) rather than raw input order, with floating
  // items placed at their reading-order anchors instead of appended after
  // the page's prose.
  const std::vector<TextBlock> blocks = build_text_blocks(source);

  // Consensus mode only: the word-to-line index lets each emitted item name
  // the vote legs whose reading deviated inside it. Empty otherwise.
  const std::vector<uint32_t> line_of_word = consensus_word_lines(source);

  struct Placed {
    const LayoutRegion* region;
    size_t anchor;
  };
  std::vector<Placed> placed;
  for (const auto& region : source.regions) {
    if (region.label != "table" && region.label != "picture") continue;
    placed.push_back({&region, region_anchor(source, blocks, region)});
  }
  std::ranges::stable_sort(placed, [](const Placed& a, const Placed& b) {
    if (a.anchor != b.anchor) return a.anchor < b.anchor;
    if (a.region->top != b.region->top) return a.region->top < b.region->top;
    return a.region->left < b.region->left;
  });

  const std::string& layout_model = source.layout_model.empty() ? kUnnamedLayoutModel
                                                                : source.layout_model;

  // Floats and caption items emitted on this page, kept for the caption
  // attachment pass; body_order collects the page's body children in the
  // order a reader meets them.
  struct EmittedFloat {
    const LayoutRegion* region;
    std::string self_ref;
    google::protobuf::RepeatedPtrField<pipestream::document::v1::RefItem>* captions;
    google::protobuf::RepeatedPtrField<pipestream::document::v1::RefItem>* children;
  };
  std::vector<EmittedFloat> floats;
  struct EmittedCaption {
    const LayoutRegion* region;
    std::string self_ref;
    pipestream::document::v1::TextItemBase* base;
  };
  std::vector<EmittedCaption> captions;
  std::vector<std::string> body_order;

  const auto emit_float = [&](const LayoutRegion& region) {
    if (region.label == "table") {
      auto* table = output->add_tables();
      const std::string self_ref = "#/tables/" + std::to_string(cursor->table_index++);
      table->set_self_ref(self_ref);
      table->mutable_parent()->set_ref("#/body");
      table->set_content_layer(pipestream::document::v1::CONTENT_LAYER_BODY);
      table->set_label(pipestream::document::v1::DOC_ITEM_LABEL_TABLE);
      auto* provenance = table->add_prov();
      provenance->set_page_no(page_number);
      set_region_bounding_box(region, provenance->mutable_bbox());
      fill_table_data(source, region, table->mutable_data());
      add_collector_source(region.structured_cells.empty() ? "geometry" : "slanet-plus",
                           region.confidence, table->mutable_source());
      floats.push_back({&region, self_ref, table->mutable_captions(), table->mutable_children()});
      body_order.push_back(self_ref);
      return;
    }
    auto* picture = output->add_pictures();
    const std::string self_ref = "#/pictures/" + std::to_string(cursor->picture_index++);
    picture->set_self_ref(self_ref);
    picture->mutable_parent()->set_ref("#/body");
    picture->set_content_layer(pipestream::document::v1::CONTENT_LAYER_BODY);
    picture->set_label(pipestream::document::v1::DOC_ITEM_LABEL_PICTURE);
    auto* provenance = picture->add_prov();
    provenance->set_page_no(page_number);
    set_region_bounding_box(region, provenance->mutable_bbox());
    add_collector_source(layout_model, region.confidence, picture->mutable_source());
    if (!region.image_png.empty()) set_picture_image(region.image_png, picture->mutable_image());
    if (!region.figure_classes.empty()) {
      // Meta is the export contract: the canonical dialect reads item meta
      // and ignores the wire annotation list, so classes land in both. The
      // annotation stays for stream consumers reading the wire directly.
      auto* meta_classification =
          picture->mutable_meta()->mutable_classification();
      auto* classification = picture->add_annotations()->mutable_classification();
      classification->set_kind("classification");
      classification->set_provenance("figure-classifier");
      for (const auto& figure_class : region.figure_classes) {
        auto* predicted = classification->add_predicted_classes();
        predicted->set_class_name(figure_class.label);
        predicted->set_confidence(figure_class.confidence);
        auto* prediction = meta_classification->add_predictions();
        prediction->set_confidence(figure_class.confidence);
        prediction->set_created_by("figure-classifier");
        prediction->set_class_name(figure_class.label);
      }
    }
    // Decoded payloads ride on the typed barcode arm, the wire's only home,
    // plus the legacy misc-annotation struct for one release. The dialect
    // exporters derive their pipestream__barcodes projection from the typed
    // arm themselves; the producer never writes an untyped copy.
    for (const auto& barcode : region.barcodes) {
      auto* typed = picture->add_annotations()->mutable_barcode();
      typed->set_format(barcode.format);
      typed->set_value(barcode.text);
      typed->set_provenance("zxing-cpp");
      auto* misc = picture->add_annotations()->mutable_misc();
      misc->set_kind("barcode");
      auto& fields = *misc->mutable_content()->mutable_fields();
      fields["format"].set_string_value(barcode.format);
      fields["value"].set_string_value(barcode.text);
      fields["provenance"].set_string_value("zxing-cpp");
    }
    floats.push_back({&region, self_ref, picture->mutable_captions(),
                      picture->mutable_children()});
    body_order.push_back(self_ref);
  };

  const auto emit_block = [&](const TextBlock& block) {
    const LayoutRegion* region = block.region;
    // Code lines keep their line structure; prose members join with spaces.
    const char separator = region != nullptr && region->label == "code" ? '\n' : ' ';
    std::string merged;
    struct MemberSpan {
      size_t line;
      uint64_t start;
      uint64_t end;
    };
    std::vector<MemberSpan> spans;
    spans.reserve(block.lines.size());
    uint64_t code_points = 0;
    for (const size_t line_index : block.lines) {
      const auto& line = source.lines[line_index];
      if (!merged.empty()) {
        merged.push_back(separator);
        ++code_points;
      }
      const uint64_t length = utf8_codepoint_count(line.text);
      spans.push_back({line_index, code_points, code_points + length});
      code_points += length;
      merged += line.text;
    }
    if (code_points > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      throw std::length_error("Text block exceeds document charspan range");
    }

    const std::string self_ref = "#/texts/" + std::to_string(cursor->text_index++);
    bool recognized_label = true;
    const auto label = region == nullptr
                           ? pipestream::document::v1::DOC_ITEM_LABEL_TEXT
                           : label_for_region(region->label, &recognized_label);
    auto* item = output->add_texts();
    // Each structural label takes its dedicated arm so the fields only that
    // arm carries (heading level, list marker, code language) can ever be
    // populated. CodeItem keeps its fields inline instead of a nested base,
    // so it is staged in a local base and transcribed below.
    pipestream::document::v1::TextItemBase staged_code_base;
    pipestream::document::v1::CodeItem* code_item = nullptr;
    pipestream::document::v1::TextItemBase* base = nullptr;
    switch (label) {
      case pipestream::document::v1::DOC_ITEM_LABEL_TITLE:
        base = item->mutable_title()->mutable_base();
        break;
      case pipestream::document::v1::DOC_ITEM_LABEL_SECTION_HEADER:
        // Level stays unset here; assign_section_header_levels clusters the
        // whole document's heading heights once every page is in.
        base = item->mutable_section_header()->mutable_base();
        break;
      case pipestream::document::v1::DOC_ITEM_LABEL_LIST_ITEM:
        base = item->mutable_list_item()->mutable_base();
        break;
      case pipestream::document::v1::DOC_ITEM_LABEL_FORMULA:
        base = item->mutable_formula()->mutable_base();
        break;
      case pipestream::document::v1::DOC_ITEM_LABEL_CODE:
        code_item = item->mutable_code();
        base = &staged_code_base;
        break;
      default:
        base = item->mutable_text()->mutable_base();
        break;
    }
    const bool furniture = is_furniture_region(region);
    base->set_self_ref(self_ref);
    base->mutable_parent()->set_ref(furniture ? "#/furniture" : "#/body");
    base->set_content_layer(furniture ? pipestream::document::v1::CONTENT_LAYER_FURNITURE
                                      : pipestream::document::v1::CONTENT_LAYER_BODY);
    base->set_label(label);
    // A label outside every known vocabulary falls back to TEXT, the
    // model's catch-all; the raw spelling rides label_raw so version skew
    // loses nothing, and the caller's warnings name the fallback instead
    // of letting a forgotten label map invisibly.
    if (region != nullptr && !recognized_label) {
      base->set_label_raw(region->label);
      if (warnings != nullptr) {
        warnings->push_back("page " + std::to_string(page_number)
                            + ": unknown region label '" + region->label
                            + "' mapped to TEXT; the raw label is kept on "
                              "label_raw");
      }
    }
    base->set_orig(merged);
    base->set_text(merged);
    // One provenance entry per member line: its own box, its own charspan
    // into the merged text (code points), so nothing about where each line
    // sat on the page is lost to the merge.
    for (const auto& span : spans) {
      const auto& member = source.lines[span.line];
      auto* provenance = base->add_prov();
      provenance->set_page_no(page_number);
      provenance->mutable_charspan()->set_start(static_cast<int32_t>(span.start));
      provenance->mutable_charspan()->set_end(static_cast<int32_t>(span.end));
      const AxisAlignedBox box = bounding_box(member);
      set_bounding_box(box, provenance->mutable_bbox());
      // Rotated or skewed lines keep their exact quad; an axis-aligned quad
      // adds nothing over the box and is skipped.
      if (!polygon_is_axis_aligned(member.polygon, box)) {
        for (const auto& vertex : member.polygon) {
          auto* point = provenance->add_polygon();
          point->set_x(vertex.x);
          point->set_y(vertex.y);
        }
      }
    }

    // Digital text declares its fonts; consecutive members sharing one font
    // fold into a single run so the span list stays proportional to the
    // formatting, not the line count. Bold and italic read off the face
    // name, the only place a text layer states them.
    for (size_t begin = 0; begin < spans.size();) {
      const auto& first = source.lines[spans[begin].line];
      size_t end = begin + 1;
      while (end < spans.size()) {
        const auto& next = source.lines[spans[end].line];
        if (next.font_name != first.font_name || next.font_size_pt != first.font_size_pt) break;
        ++end;
      }
      if (first.font_name.has_value() || first.font_size_pt.has_value()) {
        auto* run = base->add_spans();
        run->mutable_range()->set_start(static_cast<int32_t>(spans[begin].start));
        run->mutable_range()->set_end(static_cast<int32_t>(spans[end - 1].end));
        if (first.font_name.has_value()) {
          run->set_font_family(*first.font_name);
          if (first.font_name->contains("Bold")) run->mutable_formatting()->set_bold(true);
          if (first.font_name->contains("Italic") || first.font_name->contains("Oblique")) {
            run->mutable_formatting()->set_italic(true);
          }
        }
        if (first.font_size_pt.has_value()) run->set_font_size_pt(*first.font_size_pt);
      }
      begin = end;
    }

    if (cursor->has_text) ++cursor->utf_offset;
    auto* offset = output->add_text_offsets();
    offset->set_self_ref(self_ref);
    offset->set_utf_start(cursor->utf_offset);
    cursor->utf_offset += code_points;
    offset->set_utf_end(cursor->utf_offset);
    cursor->has_text = true;

    // The row's confidence is the weakest member's; a block is only as
    // trustworthy as its shakiest line.
    std::optional<float> confidence;
    bool digital = false;
    bool ocr = false;
    for (const size_t line_index : block.lines) {
      const auto& line = source.lines[line_index];
      if (line.confidence.has_value()) {
        confidence = confidence.has_value() ? std::min(*confidence, *line.confidence)
                                            : *line.confidence;
      }
      if (text_source_for(source, line) == pipestream::parse::v1::TEXT_SOURCE_DIGITAL_PDF) {
        digital = true;
      } else {
        ocr = true;
      }
    }
    if (confidence.has_value()) offset->set_confidence(*confidence);
    // A uniform block keeps its origin; mixed digital and OCR members report
    // OCR, the weaker claim.
    offset->set_source(digital && !ocr ? pipestream::parse::v1::TEXT_SOURCE_DIGITAL_PDF
                                       : pipestream::parse::v1::TEXT_SOURCE_OCR);
    // Digital text is the PDF's own text layer, read through the configured
    // PDF backend service; the consensus attribution below names the engine
    // when several voted.
    if (digital) add_collector_source("pdf-text", confidence, base->mutable_source());
    if (ocr) add_collector_source("rapidocr", confidence, base->mutable_source());

    // Consensus attribution (PDF multi-backend votes only): when a losing
    // leg's reading deviated inside this item, the item names the vote as
    // the source of its carried text, and each deviating loser beside it.
    // The losing words themselves stay on PageData.reconciliation. CodeItem
    // carries no field_sources arm, so code blocks skip attribution.
    if (code_item == nullptr && source.vote.has_value()) {
      const std::vector<std::string> deviating =
          deviating_legs(source, block.lines, line_of_word);
      if (!deviating.empty()) {
        auto* holder = base->add_field_sources();
        holder->set_field("text");
        auto* holder_source = holder->mutable_source();
        holder_source->set_collector("protomolt");
        holder_source->set_model(leg_name(source.vote->winner));
        holder_source->set_raw_score(source.vote->winner_score);
        holder_source->set_raw_score_kind("consensus_bigram_agreement");
        for (const std::string& loser : deviating) {
          auto* entry = base->add_field_sources();
          entry->set_field("text");
          entry->mutable_source()->set_collector(loser);
        }
      }
    }

    if (code_item != nullptr) {
      // Transcribe the staged base into CodeItem's inline mirror of the same
      // fields; the field numbers match by design and the schema notes keep
      // them matching.
      code_item->set_self_ref(staged_code_base.self_ref());
      *code_item->mutable_parent() = staged_code_base.parent();
      code_item->set_content_layer(staged_code_base.content_layer());
      code_item->set_label(staged_code_base.label());
      *code_item->mutable_prov() = staged_code_base.prov();
      code_item->set_orig(staged_code_base.orig());
      code_item->set_text(staged_code_base.text());
      *code_item->mutable_source() = staged_code_base.source();
    }

    // Never register a caption through the staged code base: it is a local.
    // (A caption region cannot map to CODE, so this only documents intent.)
    if (code_item == nullptr && region != nullptr && region->label == "caption") {
      captions.push_back({region, self_ref, base});
    }
    if (!furniture) body_order.push_back(self_ref);
  };

  size_t next_placed = 0;
  for (size_t index = 0; index <= blocks.size(); ++index) {
    while (next_placed < placed.size() && placed[next_placed].anchor == index) {
      emit_float(*placed[next_placed].region);
      ++next_placed;
    }
    if (index == blocks.size()) break;
    if (!blocks[index].suppressed) emit_block(blocks[index]);
  }

  // A caption binds to the nearest table or picture it visually labels: at
  // least 30% of the caption's width overlapping horizontally and a vertical
  // gap of at most 1.5 caption heights, nearest gap wins. The claimed
  // caption re-parents under the float and leaves body order; renderers then
  // emit it with its float instead of as free prose. The link is written on
  // both sides, as docling-core's add_text(parent=float) does: the caption's
  // parent names the float, and the float lists the caption among its
  // children as well as its captions. A parent that does not list its child
  // is the one-sided link the integrity check reports and the opt-in
  // repair_referenced_orphans pass exists to mend in foreign documents; a
  // document this assembler produces never needs it.
  for (auto& caption : captions) {
    const double width = caption.region->right - caption.region->left;
    const double height = caption.region->bottom - caption.region->top;
    if (width <= 0 || height <= 0) continue;
    const EmittedFloat* best = nullptr;
    double best_gap = 0;
    for (const auto& target : floats) {
      const double overlap = std::min(caption.region->right, target.region->right) -
                             std::max(caption.region->left, target.region->left);
      if (overlap < 0.3 * width) continue;
      double gap = 0;
      if (caption.region->top >= target.region->bottom) {
        gap = caption.region->top - target.region->bottom;
      } else if (caption.region->bottom <= target.region->top) {
        gap = target.region->top - caption.region->bottom;
      }
      if (gap > 1.5 * height) continue;
      if (best == nullptr || gap < best_gap) {
        best = &target;
        best_gap = gap;
      }
    }
    if (best == nullptr) continue;
    caption.base->mutable_parent()->set_ref(best->self_ref);
    best->captions->Add()->set_ref(caption.self_ref);
    best->children->Add()->set_ref(caption.self_ref);
    std::erase(body_order, caption.self_ref);
  }

  for (const auto& ref : body_order) output->add_body_order()->set_ref(ref);
}

void append_page_to_document(
    const OcrPage& source, int page_number, AssemblyCursor* cursor,
    pipestream::document::v1::Document* document, std::string* plain_text,
    google::protobuf::RepeatedPtrField<pipestream::parse::v1::TextOffset>* text_offsets,
    std::vector<std::string>* warnings) {
  if (document == nullptr || plain_text == nullptr) {
    throw std::invalid_argument("Document assembly output is required");
  }
  pipestream::parse::v1::PageData page;
  append_page_data(source, page_number, cursor, &page, warnings);
  if (text_offsets != nullptr) {
    for (auto& offset : *page.mutable_text_offsets()) {
      *text_offsets->Add() = std::move(offset);
    }
  }
  (*document->mutable_pages())[page_number] = std::move(*page.mutable_page_meta());

  // With a body order the page names its own body children (captions a
  // float claimed and furniture rows already absent); without one, legacy
  // producers fall back to texts-tables-pictures order.
  const bool ordered = page.body_order_size() > 0;

  auto* texts = page.mutable_texts();
  document->mutable_texts()->Reserve(document->texts_size() + texts->size());
  document->mutable_body()->mutable_children()->Reserve(document->body().children_size() +
                                                        texts->size());
  for (auto& text : *texts) {
    // Read everything needed from `text` before it is moved out. CodeItem
    // keeps its fields inline instead of a nested base, so it is read here
    // rather than through the shared accessor.
    const auto* base = render::text_base(text);
    std::string_view item_text;
    std::string_view item_self_ref;
    auto content_layer = pipestream::document::v1::CONTENT_LAYER_BODY;
    bool known = false;
    if (base != nullptr) {
      item_text = base->text();
      item_self_ref = base->self_ref();
      content_layer = base->content_layer();
      known = true;
    } else if (text.item_case() == pipestream::document::v1::BaseTextItem::kCode) {
      item_text = text.code().text();
      item_self_ref = text.code().self_ref();
      content_layer = text.code().content_layer();
      known = true;
    }
    if (known) {
      const bool furniture =
          content_layer == pipestream::document::v1::CONTENT_LAYER_FURNITURE;
      if (furniture) {
        document->mutable_furniture()->add_children()->set_ref(std::string(item_self_ref));
      } else if (!ordered) {
        document->mutable_body()->add_children()->set_ref(std::string(item_self_ref));
      }
      if (!plain_text->empty()) plain_text->push_back('\n');
      plain_text->append(item_text);
    }
    // Hand the item over instead of deep-copying every box and string again.
    *document->add_texts() = std::move(text);
  }

  for (auto& table : *page.mutable_tables()) {
    if (!ordered) document->mutable_body()->add_children()->set_ref(table.self_ref());
    *document->add_tables() = std::move(table);
  }
  for (auto& picture : *page.mutable_pictures()) {
    if (!ordered) document->mutable_body()->add_children()->set_ref(picture.self_ref());
    *document->add_pictures() = std::move(picture);
  }
  for (const auto& ref : page.body_order()) {
    document->mutable_body()->add_children()->set_ref(ref.ref());
  }
}

int group_list_items(pipestream::document::v1::Document* document) {
  if (document == nullptr) throw std::invalid_argument("Document assembly output is required");
  // The texts index of a body child that is a list item parented to the
  // body itself, or nothing.
  const auto body_list_item = [document](const std::string& ref) -> std::optional<int> {
    constexpr std::string_view kTexts = "#/texts/";
    if (!ref.starts_with(kTexts)) return std::nullopt;
    int index = 0;
    const char* last = ref.data() + ref.size();
    const auto [end, error] = std::from_chars(ref.data() + kTexts.size(), last, index);
    if (error != std::errc() || end != last || index < 0 || index >= document->texts_size()) {
      return std::nullopt;
    }
    const auto& text = document->texts(index);
    if (text.item_case() != pipestream::document::v1::BaseTextItem::kListItem ||
        text.list_item().base().parent().ref() != "#/body") {
      return std::nullopt;
    }
    return index;
  };
  google::protobuf::RepeatedPtrField<pipestream::document::v1::RefItem> children;
  int made = 0;
  const auto& body = document->body().children();
  for (int at = 0; at < body.size();) {
    if (!body_list_item(body[at].ref()).has_value()) {
      *children.Add() = body[at++];
      continue;
    }
    const std::string group_ref = "#/groups/" + std::to_string(document->groups_size());
    auto* group = document->add_groups();
    group->set_self_ref(group_ref);
    group->mutable_parent()->set_ref("#/body");
    group->set_content_layer(pipestream::document::v1::CONTENT_LAYER_BODY);
    group->set_label(pipestream::document::v1::GROUP_LABEL_LIST);
    group->set_name("list");
    for (; at < body.size(); ++at) {
      const std::optional<int> index = body_list_item(body[at].ref());
      if (!index.has_value()) break;
      document->mutable_texts(*index)->mutable_list_item()->mutable_base()->mutable_parent()->set_ref(
          group_ref);
      group->add_children()->set_ref(body[at].ref());
    }
    children.Add()->set_ref(group_ref);
    ++made;
  }
  document->mutable_body()->mutable_children()->Swap(&children);
  return made;
}

void append_consensus_claim(const std::vector<const OcrPage*>& pages,
                            pipestream::document::v1::Document* document) {
  if (document == nullptr) throw std::invalid_argument("Document assembly output is required");
  size_t voted = 0;
  double score_sum = 0.0;
  std::string winner;
  bool unanimous = true;
  for (const OcrPage* page : pages) {
    if (page == nullptr || !page->vote.has_value()) continue;
    const std::string name = leg_name(page->vote->winner);
    if (voted == 0) {
      winner = name;
    } else if (name != winner) {
      unanimous = false;
    }
    score_sum += page->vote->winner_score;
    ++voted;
  }
  // No page voted: single-backend documents and single-candidate pages
  // carry no claim, exactly as before.
  if (voted == 0) return;
  // One claim per document, under the vote's own claimant name. The
  // per-page word detail already rides PageData.reconciliation; repeating
  // the vote per page would multiply the resolved document by its page
  // count for nothing a consumer cannot already read there. The score is
  // the mean of the per-page winning scores over the pages that voted,
  // with that page count as its samples; it is the vote's own composite
  // (bigram agreement plus sentence continuity), not a probability, so it
  // rides raw_score with its kind named, never confidence.
  auto* claim = document->add_claims();
  auto* source = claim->mutable_source();
  source->set_collector("protomolt");
  // The winning engine is named only when every voted page picked it; a
  // split document leaves model unset rather than letting one page
  // subset's winner speak for the whole.
  if (unanimous) source->set_model(winner);
  source->set_raw_score(score_sum / static_cast<double>(voted));
  source->set_raw_score_kind("consensus_bigram_agreement");
  source->set_raw_score_samples(static_cast<uint64_t>(voted));
}

}  // namespace grparse
