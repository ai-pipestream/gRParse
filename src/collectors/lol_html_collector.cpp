#include "grparse/document_collectors.h"

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/util/json_util.h>

#include "collector_support.h"
#include "grparse/document_assembly.h"
#include "lolhtml/v1/lolhtml_service.grpc.pb.h"

namespace docv1 = ai::pipestream::document::v1;
namespace lolv1 = lolhtml::v1;

namespace grparse {
namespace {

// The one collector wire with no document event: the match stream IS the
// product, so the fold happens here. One group per rule, its matches and
// text as source-tagged text items in arrival order. Instance nesting is
// not reconstructed: matches are a transcript, not a tree; a caller who
// wants document structure runs the markup collector instead.
class LolHtmlFold {
 public:
  explicit LolHtmlFold(docv1::Document& document) : document_(document) {
    document_.mutable_body()->set_self_ref("#/body");
    document_.mutable_body()->set_content_layer(docv1::CONTENT_LAYER_BODY);
    document_.mutable_furniture()->set_self_ref("#/furniture");
    document_.mutable_furniture()->set_content_layer(docv1::CONTENT_LAYER_FURNITURE);
  }

  void element(const lolv1::ElementMatched& matched) {
    std::string tag =
        matched.tag_name().empty() ? matched.tag_name_raw() : matched.tag_name();
    tag_by_rule_[matched.rule_id()] = tag;
    capture_page_identity(tag, matched);
    std::vector<Link> links;
    docv1::TextItemBase* base = add_text(matched.rule_id(), pseudo_tag(tag, matched, &links));
    for (const auto& [range, href] : links) {
      docv1::InlineSpan* span = base->add_spans();
      span->mutable_range()->set_start(static_cast<int32_t>(range.first));
      span->mutable_range()->set_end(static_cast<int32_t>(range.second));
      span->set_hyperlink(href);
    }
    // The house convention keeps the first link in the item's scalar slot as
    // the primary one; the spans carry every link with its position.
    if (!links.empty()) base->set_hyperlink(links.front().second);
  }

  // A text node names only the rule that produced it, so the title has to be
  // recognized from the element event that preceded it on the same rule.
  // Without CAPTURE_TAG_NAME there is nothing to recognize and the title
  // stays an ordinary text item, which is the honest outcome.
  void text_node(const lolv1::TextNode& node) {
    add_text(node.rule_id(), node.text());
    const auto matched = tag_by_rule_.find(node.rule_id());
    if (matched == tag_by_rule_.end() || matched->second != "title" ||
        node.text().empty() || document_.source_meta().has_title()) {
      return;
    }
    // The page's own title: the document's name, its declared title, and a
    // page-level pair beside the <meta> ones.
    document_.set_name(node.text());
    document_.mutable_source_meta()->set_title(node.text());
    docv1::MetaTag* tag_pair = document_.add_meta_tags();
    tag_pair->set_name("title");
    tag_pair->set_content(node.text());
  }

  void comment(const lolv1::CommentFound& found) {
    add_text(found.rule_id(), "<!--" + found.text() + "-->");
  }

  void doctype(const lolv1::DoctypeFound& found) {
    std::string text = "<!DOCTYPE";
    if (found.has_name()) text += " " + found.name();
    text += ">";
    add_text(found.rule_id(), std::move(text));
  }

 private:
  // One hyperlink run: the code-point range it occupies in the item's text,
  // and the href itself.
  using Link = std::pair<std::pair<uint64_t, uint64_t>, std::string>;

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

  docv1::TextItemBase* add_text(const std::string& rule, std::string text) {
    const int group = group_ref(rule);
    auto* base = document_.add_texts()->mutable_text()->mutable_base();
    base->set_self_ref("#/texts/" + std::to_string(document_.texts_size() - 1));
    base->mutable_parent()->set_ref("#/groups/" + std::to_string(group));
    base->set_content_layer(docv1::CONTENT_LAYER_BODY);
    base->set_label(docv1::DOC_ITEM_LABEL_TEXT);
    base->set_orig(text);
    base->set_text(std::move(text));
    base->add_source()->mutable_collector()->set_collector("lol-html");
    document_.mutable_groups(group)->add_children()->set_ref(base->self_ref());
    return base;
  }

  // The verbatim transcript of one element's start tag, plus the hyperlink
  // runs it carries. href arrives typed, so it stays typed: each one becomes
  // an InlineSpan hyperlink run over the characters its value occupies in
  // the pseudo-tag. Ranges are code points into the item text, as InlineSpan
  // declares, so a non-ASCII attribute earlier in the tag does not shift
  // them.
  static std::string pseudo_tag(const std::string& tag,
                                const lolv1::ElementMatched& matched,
                                std::vector<Link>* links) {
    constexpr auto kSpanLimit = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    std::string text = "<" + (tag.empty() ? "match" : tag);
    uint64_t points = utf8_codepoint_count(text);
    for (const auto& attribute : matched.attributes()) {
      const std::string opening = " " + attribute.name() + "=\"";
      text += opening;
      points += utf8_codepoint_count(opening);
      const uint64_t value_start = points;
      text += attribute.value();
      points += utf8_codepoint_count(attribute.value());
      text += "\"";
      const uint64_t value_end = points;
      points += 1;
      // A range wider than the span type can hold is left off rather than
      // truncated into a lie about where the link sits.
      if (attribute.name() == "href" && !attribute.value().empty() &&
          value_end <= kSpanLimit) {
        links->emplace_back(std::pair{value_start, value_end}, attribute.value());
      }
    }
    text += ">";
    return text;
  }

  // Attribute lookup on a match, by the normalized (lower-cased) name the
  // wire guarantees; absent means the element did not carry it.
  static std::optional<std::string> attribute_value(const lolv1::ElementMatched& matched,
                                                    std::string_view name) {
    for (const auto& attribute : matched.attributes()) {
      if (attribute.name() == name) return attribute.value();
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
      if (rel && href && !href->empty() && rel->contains("canonical") &&
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
    // it uses; MetaTag.name keeps the spelling the page wrote.
    const auto content = attribute_value(matched, "content");
    if (!content) return;
    for (const std::string_view key : {"name", "property", "http-equiv"}) {
      const auto name = attribute_value(matched, key);
      if (!name || name->empty()) continue;
      docv1::MetaTag* tag_pair = document_.add_meta_tags();
      tag_pair->set_name(*name);
      tag_pair->set_content(*content);
      return;
    }
  }

  docv1::Document& document_;
  std::map<std::string, int> groups_by_rule_;
  std::map<std::string, std::string> tag_by_rule_;
};

// The run's own report: a bail-out, and every rule that matched nothing. A
// rule that matched nothing is worth saying out loud, because an empty group
// and a mistyped selector look identical otherwise.
void note_finished(const lolv1::ExtractFinished& finished,
                   std::vector<std::string>* warnings) {
  if (finished.bailed_out()) {
    warnings->push_back("bailed out before the end of the document: " +
                        finished.bail_out_reason());
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
                                           const std::string& options_json,
                                           const std::string& bytes,
                                           CollectorDeadline inbound_deadline) {
  CollectorOutcome outcome;
  if (options_json.empty()) {
    // Nothing to dial: the collector extracts what its selector rules name,
    // and this client never invents rules.
    outcome.error = "lol-html collector: a parse requires lol_html_options_json";
    outcome.code = grpc::StatusCode::INVALID_ARGUMENT;
    return outcome;
  }
  lolv1::ExtractOptions options;
  const auto parsed =
      google::protobuf::util::JsonStringToMessage(options_json, &options);
  if (!parsed.ok()) {
    outcome.error =
        "lol-html collector: lol_html_options_json does not parse as "
        "lolhtml.v1.ExtractOptions: " +
        std::string(parsed.message());
    outcome.code = grpc::StatusCode::INVALID_ARGUMENT;
    return outcome;
  }

  auto stub = lolv1::LolHtmlService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  auto stream = stub->Extract(&context);

  lolv1::ExtractRequest request;
  *request.mutable_options() = std::move(options);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](lolv1::ExtractRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.set_chunk(bytes.data() + offset, length);
                });

  LolHtmlFold fold(outcome.document);
  bool finished_seen = false;
  bool error_seen = false;
  lolv1::ExtractResponse event;
  while (stream->Read(&event)) {
    switch (event.event_case()) {
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
      case lolv1::ExtractResponse::kFinished:
        finished_seen = true;
        note_finished(event.finished(), &outcome.warnings);
        break;
      case lolv1::ExtractResponse::kError:
        // Terminal and in-band by contract; the RPC itself still ends OK.
        error_seen = true;
        note_error(event.error(), &outcome);
        break;
      case lolv1::ExtractResponse::kStarted:
      case lolv1::ExtractResponse::kEndTag:
      default:
        break;
    }
    event.Clear();
  }

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
