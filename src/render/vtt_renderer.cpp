// The WebVTT renderer behind render_vtt; semantics documented on the
// declaration in include/grparse/document_render.h.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "grparse/document_render.h"
#include "renderer_base.h"

namespace docv1 = ai::pipestream::document::v1;

namespace grparse {
namespace {

using namespace grparse::render;

// "HH:MM:SS.mmm" with two-digit fields (hours widen past 99), from
// milliseconds so 999.6 ms rounds into the seconds field instead of
// printing a four-digit millisecond count.
std::string vtt_timestamp(double seconds) {
  const long long total_ms = std::llround(std::max(seconds, 0.0) * 1000.0);
  const long long hours = total_ms / 3600000;
  const long long minutes = (total_ms / 60000) % 60;
  const long long secs = (total_ms / 1000) % 60;
  const long long millis = total_ms % 1000;
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld.%03lld", hours,
                minutes, secs, millis);
  return std::string(buffer);
}

class VttRenderer : RendererBase {
 public:
  explicit VttRenderer(const docv1::Document& document) : RendererBase(document) {}

  std::string render() {
    walk(document_.body());
    flush_cue();
    std::string out = title_.empty() ? "WEBVTT\n" : "WEBVTT " + title_ + "\n";
    for (const auto& block : blocks_) {
      out.push_back('\n');
      out.append(block);
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
  }

 private:
  std::string title_;
  std::vector<std::string> blocks_;
  // The open cue being merged: consecutive items with the same identifier
  // and timing join into one payload.
  bool cue_open_ = false;
  std::string cue_identifier_;
  double cue_start_ = 0.0;
  double cue_end_ = 0.0;
  std::string cue_payload_;

  void flush_cue() {
    if (!cue_open_) return;
    std::string block;
    if (!cue_identifier_.empty()) block.append(cue_identifier_ + "\n");
    block.append(vtt_timestamp(cue_start_) + " --> " + vtt_timestamp(cue_end_) + "\n");
    block.append(cue_payload_ + "\n");
    blocks_.push_back(std::move(block));
    cue_open_ = false;
    cue_payload_.clear();
  }

  void walk(const docv1::GroupItem& group) {
    for (const auto& child : group.children()) {
      const ArenaRef ref = parse_ref(child.ref());
      if (ref.kind == ArenaRef::kGroup && ref.index < document_.groups_size()) {
        if (consume(child.ref())) walk(document_.groups(ref.index));
        continue;
      }
      if (ref.kind != ArenaRef::kText || ref.index >= document_.texts_size()) continue;
      if (!consume(child.ref())) continue;
      handle_text(document_.texts(ref.index));
    }
  }

  // The item's first track source. Our wire also stamps CollectorSource
  // attribution into the same list, so the scan skips past those instead of
  // testing only the first entry as docling does.
  static const docv1::TrackSource* track_source(const docv1::TextItemBase& base) {
    for (const auto& source : base.source()) {
      if (source.has_track()) return &source.track();
    }
    return nullptr;
  }

  void handle_text(const docv1::BaseTextItem& item) {
    const auto* base = text_base(item);
    if (base == nullptr || furniture(base->content_layer())) return;
    if (item.item_case() == docv1::BaseTextItem::kTitle ||
        base->label() == docv1::DOC_ITEM_LABEL_TITLE) {
      if (!base->text().empty()) title_ = trimmed(base->text());
      return;
    }
    if (base->text().empty()) return;
    const docv1::TrackSource* track = track_source(*base);
    if (track == nullptr) return;

    std::string text = base->text();
    if (track->has_voice() && !track->voice().empty()) {
      text = "<v " + track->voice() + ">" + text + "</v>";
    }
    const std::string identifier = track->has_identifier() ? track->identifier() : "";
    if (cue_open_ && identifier == cue_identifier_ && track->start_time() == cue_start_ &&
        track->end_time() == cue_end_) {
      cue_payload_.append("\n" + text);
      return;
    }
    flush_cue();
    cue_open_ = true;
    cue_identifier_ = identifier;
    cue_start_ = track->start_time();
    cue_end_ = track->end_time();
    cue_payload_ = text;
  }
};

}  // namespace

std::string render_vtt(const docv1::Document& document) {
  return VttRenderer(document).render();
}

}  // namespace grparse
