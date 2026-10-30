#include "grparse/document_collectors.h"

#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "collector_support.h"
#include "grparse/document_assembly.h"
#include "lolhtml/v1/lolhtml_service.grpc.pb.h"

namespace docv1 = ai::pipestream::document::v1;
namespace lolv1 = lolhtml::v1;

namespace grparse {
namespace {

constexpr auto kSpanLimit = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());

std::string lowercase_ascii(std::string_view input) {
  std::string out(input.size(), '\0');
  for (size_t i = 0; i < input.size(); ++i) {
    out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(input[i])));
  }
  return out;
}

std::string_view trim_ascii(std::string_view input) {
  while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())) != 0) {
    input.remove_prefix(1);
  }
  while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())) != 0) {
    input.remove_suffix(1);
  }
  return input;
}

// Namespace URI for the three namespaces HTML5 parsing places elements into.
std::string_view namespace_uri(lolv1::Namespace ns) {
  switch (ns) {
    case lolv1::NAMESPACE_HTML:
      return "http://www.w3.org/1999/xhtml";
    case lolv1::NAMESPACE_SVG:
      return "http://www.w3.org/2000/svg";
    case lolv1::NAMESPACE_MATHML:
      return "http://www.w3.org/1998/Math/MathML";
    case lolv1::NAMESPACE_UNSPECIFIED:
    default:
      return {};
  }
}

// Foreign content (SVG and MathML) preserves element and attribute casing
// (viewBox, linearGradient); HTML normalizes names to lower-case.
bool is_foreign_namespace(lolv1::Namespace ns) {
  return ns == lolv1::NAMESPACE_SVG || ns == lolv1::NAMESPACE_MATHML;
}

// HTML void elements never own text content or emit an end tag, even when a
// synthetic test event omits `can_have_content`.
bool is_void_html_element(std::string_view lower_tag) {
  return lower_tag == "area" || lower_tag == "base" || lower_tag == "br" ||
         lower_tag == "col" || lower_tag == "embed" || lower_tag == "hr" ||
         lower_tag == "img" || lower_tag == "input" || lower_tag == "link" ||
         lower_tag == "meta" || lower_tag == "source" || lower_tag == "track" ||
         lower_tag == "wbr";
}

bool is_json_ld_mime(std::string_view type_attr) {
  const size_t semi = type_attr.find(';');
  const std::string_view media_type =
      trim_ascii(semi == std::string_view::npos ? type_attr : type_attr.substr(0, semi));
  return lowercase_ascii(media_type) == "application/ld+json";
}

void append_utf8_codepoint(uint32_t cp, std::string* out) {
  if (cp == 0 || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
    cp = 0xFFFDU;
  }
  if (cp <= 0x7FU) {
    out->push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFU) {
    out->push_back(static_cast<char>(0xC0U | (cp >> 6U)));
    out->push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  } else if (cp <= 0xFFFFU) {
    out->push_back(static_cast<char>(0xE0U | (cp >> 12U)));
    out->push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
    out->push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  } else {
    out->push_back(static_cast<char>(0xF0U | (cp >> 18U)));
    out->push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
    out->push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
    out->push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  }
}

// Decodes HTML character references when `raw_text_chunks` streams un-decoded
// lexer fragments for DATA and RCDATA text nodes.
std::string decode_html_entities(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] != '&') {
      out.push_back(input[i]);
      continue;
    }
    const size_t semi = input.find(';', i + 1);
    if (semi == std::string_view::npos || semi - i > 32) {
      out.push_back(input[i]);
      continue;
    }
    const std::string_view entity = input.substr(i + 1, semi - i - 1);
    if (entity.size() >= 2 && entity[0] == '#') {
      uint32_t cp = 0;
      bool valid = true;
      if (entity[1] == 'x' || entity[1] == 'X') {
        if (entity.size() == 2) valid = false;
        for (size_t k = 2; k < entity.size(); ++k) {
          const auto c = static_cast<unsigned char>(entity[k]);
          if (std::isxdigit(c) == 0) {
            valid = false;
            break;
          }
          const uint32_t digit =
              std::isdigit(c) != 0 ? static_cast<uint32_t>(c - '0')
                                   : static_cast<uint32_t>(std::tolower(c) - 'a' + 10);
          cp = cp <= 0x10FFFFU ? (cp * 16U + digit) : 0x110000U;
        }
      } else {
        for (size_t k = 1; k < entity.size(); ++k) {
          const auto c = static_cast<unsigned char>(entity[k]);
          if (std::isdigit(c) == 0) {
            valid = false;
            break;
          }
          cp = cp <= 0x10FFFFU ? (cp * 10U + static_cast<uint32_t>(c - '0')) : 0x110000U;
        }
      }
      if (valid) {
        append_utf8_codepoint(cp, &out);
        i = semi;
        continue;
      }
    } else if (entity == "amp") {
      out.push_back('&');
      i = semi;
      continue;
    } else if (entity == "lt") {
      out.push_back('<');
      i = semi;
      continue;
    } else if (entity == "gt") {
      out.push_back('>');
      i = semi;
      continue;
    } else if (entity == "quot") {
      out.push_back('"');
      i = semi;
      continue;
    } else if (entity == "apos" || entity == "#39") {
      out.push_back('\'');
      i = semi;
      continue;
    } else if (entity == "nbsp") {
      out.append("\xC2\xA0");
      i = semi;
      continue;
    } else if (entity == "copy") {
      out.append("\xC2\xA9");
      i = semi;
      continue;
    } else if (entity == "reg") {
      out.append("\xC2\xAE");
      i = semi;
      continue;
    } else if (entity == "ndash") {
      out.append("\xE2\x80\x93");
      i = semi;
      continue;
    } else if (entity == "mdash") {
      out.append("\xE2\x80\x94");
      i = semi;
      continue;
    } else if (entity == "hellip") {
      out.append("\xE2\x80\xA6");
      i = semi;
      continue;
    }
    out.push_back(input[i]);
  }
  return out;
}

void add_byte_provenance(docv1::TextItemBase* base, const lolv1::SourceSpan& span,
                         uint64_t cp_start, uint64_t cp_end) {
  docv1::ProvenanceItem* prov = base->add_prov();
  prov->mutable_byte_range()->set_start(span.start());
  prov->mutable_byte_range()->set_end(span.end());
  if (cp_end <= kSpanLimit) {
    prov->mutable_charspan()->set_start(static_cast<int32_t>(cp_start));
    prov->mutable_charspan()->set_end(static_cast<int32_t>(cp_end));
  }
}

// The one collector wire with no document event: the match stream IS the
// product, so the fold happens here. One group per rule, its matches and
// text as source-tagged text items in arrival order. Instance nesting is
// not reconstructed: matches are a transcript, not a tree; a caller who
// wants document structure runs the markup collector instead.
class LolHtmlFold {
 public:
  LolHtmlFold(docv1::Document& document, const lolv1::ExtractOptions& options)
      : document_(document), raw_text_chunks_(options.raw_text_chunks()) {
    document_.mutable_body()->set_self_ref("#/body");
    document_.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
    document_.mutable_furniture()->set_self_ref("#/furniture");
    document_.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
    for (const auto& rule : options.rules()) {
      if (lowercase_ascii(rule.selector()).contains("application/ld+json")) {
        json_ld_rules_.emplace(rule.id(), true);
      }
    }
  }

  void started(const lolv1::ExtractStarted& started) {
    if (!started.encoding().empty()) {
      (*document_.mutable_body()
            ->mutable_meta()
            ->mutable_custom_fields())["lol_html.encoding"]
          .set_string_value(started.encoding());
    }
  }

  void element(const lolv1::ElementMatched& matched) {
    flush_raw_text(matched.rule_id());
    const bool foreign = is_foreign_namespace(matched.namespace_());
    const std::string lower_tag = !matched.tag_name().empty()
                                      ? matched.tag_name()
                                      : lowercase_ascii(matched.tag_name_raw());
    const std::string display_tag =
        (foreign && !matched.tag_name_raw().empty())
            ? matched.tag_name_raw()
            : (!matched.tag_name().empty() ? matched.tag_name() : matched.tag_name_raw());

    capture_page_identity(lower_tag, matched);

    const bool can_contain =
        !matched.self_closing() && !is_void_html_element(lower_tag) &&
        (matched.can_have_content() || matched.namespace_() == lolv1::NAMESPACE_UNSPECIFIED);
    if (can_contain) {
      bool is_json_ld = json_ld_rules_.contains(matched.rule_id());
      if (lower_tag == "script") {
        if (const auto type = attribute_value(matched, "type")) {
          is_json_ld = is_json_ld_mime(*type);
        }
      }
      element_by_rule_[matched.rule_id()] = ActiveElement{
          .lower_tag = lower_tag,
          .display_tag = display_tag,
          .ns = matched.namespace_(),
          .is_json_ld = is_json_ld,
      };
    } else {
      element_by_rule_.erase(matched.rule_id());
    }

    std::vector<Link> links;
    std::vector<AttrProv> attr_provs;
    uint64_t total_points = 0;
    std::string orig;
    std::string rendered =
        pseudo_tag(display_tag, matched, &orig, &links, &attr_provs, &total_points);
    docv1::TextItemBase* base =
        add_text(matched.rule_id(), std::move(rendered), std::move(orig));
    stamp_element_origin(base, display_tag, matched.namespace_());

    if (matched.has_span()) {
      add_byte_provenance(base, matched.span(), 0, total_points);
    }
    for (const auto& [span, cp_start, cp_end] : attr_provs) {
      add_byte_provenance(base, span, cp_start, cp_end);
    }

    const auto link_title = attribute_value(matched, "title");
    const auto lang = attribute_value(matched, "lang");
    for (const auto& [range, href] : links) {
      docv1::InlineSpan* span = base->add_spans();
      span->mutable_range()->set_start(static_cast<int32_t>(range.first));
      span->mutable_range()->set_end(static_cast<int32_t>(range.second));
      span->set_hyperlink(href);
      if (link_title && !link_title->empty()) span->set_link_title(*link_title);
      if (lang && !lang->empty()) span->set_language(*lang);
    }
    // The house convention keeps the first link in the item's scalar slot as
    // the primary one; the spans carry every link with its position.
    if (!links.empty()) base->set_hyperlink(links.front().second);
  }

  // A text node names only the rule that produced it, so the enclosing
  // element is recognized from the element event that preceded it on the
  // same rule. When `raw_text_chunks` is enabled on the request, raw lexer
  // fragments accumulate per rule until `last_in_node` closes the node.
  void text_node(const lolv1::TextNode& node) {
    if (raw_text_chunks_) {
      RawTextBuffer& buf = raw_text_by_rule_[node.rule_id()];
      if (!buf.active) {
        buf.active = true;
        buf.text_type = node.text_type();
      }
      buf.raw += node.text();
      if (node.has_span()) {
        if (!buf.has_span) {
          buf.span = node.span();
          buf.has_span = true;
        } else {
          buf.span.set_end(node.span().end());
        }
      }
      if (node.last_in_node()) {
        flush_raw_text(node.rule_id());
      }
      return;
    }
    if (node.text().empty() && node.last_in_node()) return;
    std::optional<lolv1::SourceSpan> span;
    if (node.has_span()) span = node.span();
    emit_text_node(node.rule_id(), node.text(), node.text(), node.text_type(), span);
  }

  void comment(const lolv1::CommentFound& found) {
    flush_raw_text(found.rule_id());
    std::string rendered = "<!--" + found.text() + "-->";
    const uint64_t points = utf8_codepoint_count(rendered);
    docv1::TextItemBase* base = add_text(found.rule_id(), std::move(rendered));
    if (found.has_span()) {
      add_byte_provenance(base, found.span(), 0, points);
    }
  }

  void doctype(const lolv1::DoctypeFound& found) {
    flush_raw_text(found.rule_id());
    std::string text = "<!DOCTYPE";
    if (found.has_name() && !found.name().empty()) {
      text += " " + found.name();
    }
    if (found.has_public_id() && !found.public_id().empty()) {
      text += " PUBLIC \"" + found.public_id() + "\"";
      if (found.has_system_id() && !found.system_id().empty()) {
        text += " \"" + found.system_id() + "\"";
      }
    } else if (found.has_system_id() && !found.system_id().empty()) {
      text += " SYSTEM \"" + found.system_id() + "\"";
    }
    text += ">";
    const uint64_t points = utf8_codepoint_count(text);
    docv1::TextItemBase* base = add_text(found.rule_id(), std::move(text));
    if (found.has_span()) {
      add_byte_provenance(base, found.span(), 0, points);
    }
  }

  void end_tag(const lolv1::EndTagFound& found) {
    flush_raw_text(found.rule_id());
    lolv1::Namespace ns = lolv1::NAMESPACE_UNSPECIFIED;
    if (const auto active = element_by_rule_.find(found.rule_id());
        active != element_by_rule_.end()) {
      ns = active->second.ns;
      element_by_rule_.erase(active);
    }
    const bool foreign = is_foreign_namespace(ns);
    const std::string tag =
        (foreign && !found.name_raw().empty())
            ? found.name_raw()
            : (!found.name().empty() ? found.name() : found.name_raw());
    const std::string raw_tag = !found.name_raw().empty() ? found.name_raw() : tag;
    std::string rendered = "</" + (tag.empty() ? "match" : tag) + ">";
    std::string orig = "</" + (raw_tag.empty() ? "match" : raw_tag) + ">";
    const uint64_t points = utf8_codepoint_count(rendered);
    docv1::TextItemBase* base =
        add_text(found.rule_id(), std::move(rendered), std::move(orig));
    stamp_element_origin(base, tag, ns);
    if (found.has_span()) {
      add_byte_provenance(base, found.span(), 0, points);
    }
  }

  void finish() {
    for (auto& [rule_id, _] : raw_text_by_rule_) {
      flush_raw_text(rule_id);
    }
  }

 private:
  // One hyperlink run: the code-point range it occupies in the item's text,
  // and the href itself.
  using Link = std::pair<std::pair<uint64_t, uint64_t>, std::string>;

  struct AttrProv {
    lolv1::SourceSpan span;
    uint64_t cp_start = 0;
    uint64_t cp_end = 0;
  };

  struct ActiveElement {
    std::string lower_tag;
    std::string display_tag;
    lolv1::Namespace ns = lolv1::NAMESPACE_UNSPECIFIED;
    bool is_json_ld = false;
  };

  struct RawTextBuffer {
    bool active = false;
    std::string raw;
    lolv1::TextType text_type = lolv1::TEXT_TYPE_UNSPECIFIED;
    bool has_span = false;
    lolv1::SourceSpan span;
  };

  static void stamp_element_origin(docv1::TextItemBase* base,
                                   const std::string& element_name,
                                   lolv1::Namespace ns) {
    if (!element_name.empty()) {
      base->set_source_element_name(element_name);
    }
    if (const std::string_view uri = namespace_uri(ns); !uri.empty()) {
      base->set_source_namespace(std::string(uri));
    }
  }

  void flush_raw_text(const std::string& rule_id) {
    const auto found = raw_text_by_rule_.find(rule_id);
    if (found == raw_text_by_rule_.end() || !found->second.active) return;
    RawTextBuffer buf = std::move(found->second);
    raw_text_by_rule_.erase(found);
    if (buf.raw.empty()) return;
    std::string decoded =
        (buf.text_type == lolv1::TEXT_TYPE_DATA || buf.text_type == lolv1::TEXT_TYPE_RCDATA)
            ? decode_html_entities(buf.raw)
            : buf.raw;
    std::optional<lolv1::SourceSpan> span;
    if (buf.has_span) span = buf.span;
    emit_text_node(rule_id, std::move(decoded), std::move(buf.raw), buf.text_type, span);
  }

  void emit_text_node(const std::string& rule_id, std::string text, std::string orig,
                      lolv1::TextType text_type,
                      const std::optional<lolv1::SourceSpan>& span) {
    const uint64_t points = utf8_codepoint_count(text);
    docv1::TextItemBase* base = add_text(rule_id, text, std::move(orig));
    if (span.has_value()) {
      add_byte_provenance(base, *span, 0, points);
    }
    const auto active = element_by_rule_.find(rule_id);
    if (active != element_by_rule_.end()) {
      stamp_element_origin(base, active->second.display_tag, active->second.ns);
    }

    const bool is_json_ld =
        (active != element_by_rule_.end() && active->second.is_json_ld) ||
        (text_type == lolv1::TEXT_TYPE_SCRIPT_DATA && json_ld_rules_.contains(rule_id));
    if (is_json_ld) {
      const std::string_view trimmed = trim_ascii(text);
      if (!trimmed.empty()) {
        docv1::StructuredData* sd = document_.add_structured_data();
        sd->set_kind("json-ld");
        sd->set_json(std::string(trimmed));
      }
    }

    if (active == element_by_rule_.end() || active->second.lower_tag != "title" ||
        text.empty() || document_.source_meta().has_title()) {
      return;
    }
    // The page's own title: the document's name, its declared title, and a
    // page-level pair beside the <meta> ones.
    document_.set_name(text);
    document_.mutable_source_meta()->set_title(text);
    docv1::MetaTag* tag_pair = document_.add_meta_tags();
    tag_pair->set_name("title");
    tag_pair->set_content(text);
  }

  int group_ref(const std::string& rule) {
    auto found = groups_by_rule_.find(rule);
    if (found == groups_by_rule_.end()) {
      const int index = document_.groups_size();
      docv1::GroupItem* group = document_.add_groups();
      group->set_self_ref("#/groups/" + std::to_string(index));
      group->mutable_parent()->set_ref("#/body");
      group->set_content_layer(docv1::CONTENT_LAYER_BODY);
      group->set_name(rule);
      group->set_label(docv1::GROUP_LABEL_SECTION);
      document_.mutable_body()->add_children()->set_ref(group->self_ref());
      found = groups_by_rule_.emplace(rule, index).first;
    }
    return found->second;
  }

  docv1::TextItemBase* add_text(const std::string& rule, std::string text,
                                std::optional<std::string> orig = std::nullopt) {
    const int group = group_ref(rule);
    auto* base = document_.add_texts()->mutable_text()->mutable_base();
    base->set_self_ref("#/texts/" + std::to_string(document_.texts_size() - 1));
    base->mutable_parent()->set_ref("#/groups/" + std::to_string(group));
    base->set_content_layer(docv1::CONTENT_LAYER_BODY);
    base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
    base->set_orig(orig.has_value() ? std::move(*orig) : text);
    base->set_text(std::move(text));
    base->add_source()->mutable_collector()->set_collector("lol-html");
    document_.mutable_groups(group)->add_children()->set_ref(base->self_ref());
    return base;
  }

  // The verbatim transcript of one element's start tag, plus the hyperlink
  // runs and byte spans it carries. href arrives typed, so it stays typed:
  // each one becomes an InlineSpan hyperlink run over the characters its
  // value occupies in the pseudo-tag. Ranges are code points into the item
  // text, as InlineSpan declares, so a non-ASCII attribute earlier in the tag
  // does not shift them.
  static std::string pseudo_tag(const std::string& display_tag,
                                const lolv1::ElementMatched& matched,
                                std::string* orig_out,
                                std::vector<Link>* links,
                                std::vector<AttrProv>* attr_provs,
                                uint64_t* total_points) {
    const bool foreign = is_foreign_namespace(matched.namespace_());
    const std::string tag_for_text = display_tag.empty() ? "match" : display_tag;
    const std::string tag_for_orig =
        !matched.tag_name_raw().empty() ? matched.tag_name_raw() : tag_for_text;
    std::string text = "<" + tag_for_text;
    std::string orig = "<" + tag_for_orig;
    uint64_t points = utf8_codepoint_count(text);
    for (const auto& attribute : matched.attributes()) {
      const std::string& attr_name =
          (foreign && !attribute.name_raw().empty())
              ? attribute.name_raw()
              : (!attribute.name().empty() ? attribute.name() : attribute.name_raw());
      const std::string& raw_attr_name =
          !attribute.name_raw().empty() ? attribute.name_raw() : attr_name;
      text += " ";
      orig += " ";
      points += 1;
      const uint64_t name_start = points;
      text += attr_name;
      orig += raw_attr_name;
      points += utf8_codepoint_count(attr_name);
      const uint64_t name_end = points;
      if (attribute.has_name_span() && name_end <= kSpanLimit) {
        attr_provs->push_back(AttrProv{
            .span = attribute.name_span(),
            .cp_start = name_start,
            .cp_end = name_end,
        });
      }
      text += "=\"";
      orig += "=\"";
      points += 2;
      const uint64_t value_start = points;
      text += attribute.value();
      orig += attribute.value();
      points += utf8_codepoint_count(attribute.value());
      const uint64_t value_end = points;
      if (attribute.has_value_span() && value_end <= kSpanLimit) {
        attr_provs->push_back(AttrProv{
            .span = attribute.value_span(),
            .cp_start = value_start,
            .cp_end = value_end,
        });
      }
      text += "\"";
      orig += "\"";
      points += 1;
      // A range wider than the span type can hold is left off rather than
      // truncated into a lie about where the link sits.
      const std::string lower_attr =
          !attribute.name().empty() ? attribute.name() : lowercase_ascii(attribute.name_raw());
      if (lower_attr == "href" && !attribute.value().empty() && value_end <= kSpanLimit) {
        links->emplace_back(std::pair{value_start, value_end}, attribute.value());
      }
    }
    const std::string_view close = matched.self_closing() ? "/>" : ">";
    text += close;
    orig += close;
    points += close.size();
    *orig_out = std::move(orig);
    *total_points = points;
    return text;
  }

  // Attribute lookup on a match, by the normalized (lower-cased) name the
  // wire guarantees; absent means the element did not carry it.
  static std::optional<std::string> attribute_value(const lolv1::ElementMatched& matched,
                                                    std::string_view name) {
    for (const auto& attribute : matched.attributes()) {
      const std::string candidate =
          !attribute.name().empty() ? attribute.name() : lowercase_ascii(attribute.name_raw());
      if (candidate == name) return attribute.value();
    }
    return std::nullopt;
  }

  // Page-level identity the fold recognizes on sight. The wire types every
  // attribute, so these land in their typed slots instead of being smeared
  // into the pseudo-tag string with everything else; the string still
  // carries them, because it is the verbatim transcript.
  void capture_page_identity(const std::string& tag, const lolv1::ElementMatched& matched) {
    if (tag == "link") {
      const auto rel = attribute_value(matched, "rel");
      const auto href = attribute_value(matched, "href");
      // rel is a space-separated token list; canonical may sit beside others.
      if (rel && href && !href->empty() && lowercase_ascii(*rel).contains("canonical") &&
          !document_.origin().web().has_canonical_uri()) {
        document_.mutable_origin()->mutable_web()->set_canonical_uri(*href);
      }
      return;
    }
    if (tag == "html") {
      if (const auto lang = attribute_value(matched, "lang")) {
        if (!lang->empty() && !document_.source_meta().has_language()) {
          document_.mutable_source_meta()->set_language(*lang);
        }
      }
      return;
    }
    if (tag != "meta") return;
    // A <meta> is a name/content pair whichever of the three key spellings
    // it uses; MetaTag.name keeps the spelling the page wrote. A standalone
    // <meta charset="..."> is also recorded as a meta pair.
    const auto content = attribute_value(matched, "content");
    if (!content) {
      if (const auto charset = attribute_value(matched, "charset");
          charset && !charset->empty()) {
        docv1::MetaTag* tag_pair = document_.add_meta_tags();
        tag_pair->set_name("charset");
        tag_pair->set_content(*charset);
      }
      return;
    }
    for (const std::string_view key : {"name", "property", "http-equiv"}) {
      const auto name = attribute_value(matched, key);
      if (!name || name->empty()) continue;
      docv1::MetaTag* tag_pair = document_.add_meta_tags();
      tag_pair->set_name(*name);
      tag_pair->set_content(*content);
      promote_meta_to_source_meta(key, lowercase_ascii(*name), *content);
      return;
    }
  }

  void promote_meta_to_source_meta(std::string_view key, const std::string& lower_name,
                                   const std::string& content) {
    const std::string_view trimmed = trim_ascii(content);
    if (trimmed.empty()) return;
    if (lower_name == "author" || lower_name == "dc.creator" ||
        lower_name == "dcterms.creator" || lower_name == "article:author") {
      document_.mutable_source_meta()->add_authors(std::string(trimmed));
      return;
    }
    if (lower_name == "generator") {
      if (!document_.source_meta().has_generator()) {
        document_.mutable_source_meta()->set_generator(std::string(trimmed));
      }
      return;
    }
    if (lower_name == "keywords") {
      std::string_view rest = trimmed;
      while (!rest.empty()) {
        const size_t comma = rest.find(',');
        const std::string_view token =
            trim_ascii(comma == std::string_view::npos ? rest : rest.substr(0, comma));
        if (!token.empty()) {
          document_.mutable_source_meta()->add_keywords(std::string(token));
        }
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
      }
      return;
    }
    if (lower_name == "subject" || lower_name == "dc.subject" ||
        lower_name == "dcterms.subject") {
      if (!document_.source_meta().has_subject()) {
        document_.mutable_source_meta()->set_subject(std::string(trimmed));
      }
      return;
    }
    if (lower_name == "content-language" || lower_name == "language" ||
        lower_name == "dc.language" || lower_name == "dcterms.language") {
      if (!document_.source_meta().has_language()) {
        document_.mutable_source_meta()->set_language(std::string(trimmed));
      }
      if (key == "http-equiv" && !document_.origin().web().has_content_language()) {
        document_.mutable_origin()->mutable_web()->set_content_language(std::string(trimmed));
      }
    }
  }

  docv1::Document& document_;
  bool raw_text_chunks_ = false;
  std::map<std::string, int> groups_by_rule_;
  std::map<std::string, ActiveElement> element_by_rule_;
  std::map<std::string, RawTextBuffer> raw_text_by_rule_;
  std::map<std::string, bool> json_ld_rules_;
};

// The run's own report: a bail-out, and every rule that matched nothing. A
// rule that matched nothing is worth saying out loud, because an empty group
// and a mistyped selector look identical otherwise.
void note_finished(const lolv1::ExtractFinished& finished,
                   std::vector<std::string>* warnings) {
  if (finished.bailed_out()) {
    std::string prefix = "bailed out before the end of the document";
    if (finished.bytes_parsed() > 0) {
      prefix += " after " + std::to_string(finished.bytes_parsed()) + " bytes";
    }
    warnings->push_back(prefix + ": " + finished.bail_out_reason());
  }
  for (const auto& [rule, count] : finished.matches_by_rule()) {
    if (count == 0) warnings->push_back("rule '" + rule + "' matched nothing");
  }
}

// The in-band terminal error: the RPC itself still ends OK, so the outcome
// carries the collector's own code.
void note_error(const lolv1::StreamError& error, CollectorOutcome* outcome) {
  outcome->error = "lol-html collector: " + lolv1::ParseErrorCode_Name(error.code()) + ": " +
                   error.message();
  outcome->code = error.code() == lolv1::PARSE_ERROR_CODE_MEMORY_LIMIT_EXCEEDED
                      ? grpc::StatusCode::RESOURCE_EXHAUSTED
                      : grpc::StatusCode::INVALID_ARGUMENT;
}

}  // namespace

CollectorOutcome collect_lol_html_document(const std::shared_ptr<grpc::Channel>& channel,
                                           const std::optional<lolv1::ExtractOptions>& options,
                                           const std::string& bytes,
                                           CollectorDeadline inbound_deadline,
                                           CollectorCancelled cancelled) {
  CollectorOutcome outcome;
  if (!options.has_value()) {
    // Nothing to dial: the collector extracts what its selector rules name,
    // and this client never invents rules.
    outcome.error = "lol-html collector: a parse requires lol_html_options";
    outcome.code = grpc::StatusCode::INVALID_ARGUMENT;
    return outcome;
  }

  auto stub = lolv1::LolHtmlService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  const CancelWatch watch(context, std::move(cancelled));
  auto stream = stub->Extract(&context);

  lolv1::ExtractRequest request;
  *request.mutable_options() = *options;
  ConcurrentUpload upload(
      context, *stream, request, bytes, /*always_send_chunk=*/false,
      [&bytes](lolv1::ExtractRequest& frame, size_t offset, size_t length, bool /*last*/) {
        frame.set_chunk(bytes.data() + offset, length);
      });

  LolHtmlFold fold(outcome.document, *options);
  bool finished_seen = false;
  bool error_seen = false;
  lolv1::ExtractResponse event;
  while (stream->Read(&event)) {
    switch (event.event_case()) {
      case lolv1::ExtractResponse::kStarted:
        fold.started(event.started());
        break;
      case lolv1::ExtractResponse::kElement:
        fold.element(event.element());
        break;
      case lolv1::ExtractResponse::kText:
        fold.text_node(event.text());
        break;
      case lolv1::ExtractResponse::kComment:
        fold.comment(event.comment());
        break;
      case lolv1::ExtractResponse::kDoctype:
        fold.doctype(event.doctype());
        break;
      case lolv1::ExtractResponse::kEndTag:
        fold.end_tag(event.end_tag());
        break;
      case lolv1::ExtractResponse::kFinished:
        fold.finish();
        finished_seen = true;
        note_finished(event.finished(), &outcome.warnings);
        break;
      case lolv1::ExtractResponse::kError:
        // Terminal and in-band by contract; the RPC itself still ends OK.
        fold.finish();
        error_seen = true;
        note_error(event.error(), &outcome);
        break;
      default:
        break;
    }
    event.Clear();
  }

  upload.join();
  const grpc::Status status = stream->Finish();
  if (!status.ok()) {
    outcome.error = std::string("lol-html collector: ") + status.error_message();
    outcome.code = map_code(status.error_code());
    return outcome;
  }
  if (error_seen) return outcome;
  if (!finished_seen) {
    outcome.error = "lol-html collector: stream ended without a terminal event";
    outcome.code = grpc::StatusCode::UNAVAILABLE;
    return outcome;
  }
  outcome.success = true;
  return outcome;
}

}  // namespace grparse
