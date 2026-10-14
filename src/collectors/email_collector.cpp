#include "grparse/document_collectors.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ai/pipestream/email/v1/email_service.grpc.pb.h"
#include "collector_support.h"
#include "grparse/document_merge.h"

namespace docv1 = ai::pipestream::document::v1;
namespace emailv1 = ai::pipestream::email::v1;

namespace grparse {

namespace {

constexpr size_t kMaxConcurrentBodyDials = 8;

// The body index the HTML body's items belong at: before the email fold's
// "attachments" list group, so the message reads title, body, attachments
// as it does when a text/plain part is mapped. The end of the body when the
// message listed no attachments.
int body_insertion_point(const docv1::Document& document) {
  constexpr std::string_view kGroupsPrefix = "#/groups/";
  const auto& children = document.body().children();
  for (int index = 0; index < children.size(); ++index) {
    const std::string& ref = children.Get(index).ref();
    if (!ref.starts_with(kGroupsPrefix)) continue;
    const int group = std::atoi(ref.c_str() + kGroupsPrefix.size());
    if (group >= 0 && group < document.groups_size() &&
        document.groups(group).name() == "attachments") {
      return index;
    }
  }
  return children.size();
}

// Folds each HTML body through the markup collector into the email's
// Document, in MIME walk order, under one shared ceiling. A part the markup
// collector cannot parse is reported and skipped; the message never fails
// for its body.
void fold_html_bodies(const std::shared_ptr<grpc::Channel>& markup,
                      std::vector<emailv1::BodyPart> html_bodies,
                      CollectorDeadline inbound_deadline, const CollectorCancelled& cancelled,
                      CollectorOutcome* outcome) {
  if (html_bodies.empty()) return;
  const CollectorDeadline bodies_deadline =
      capped_collector_deadline(inbound_deadline, kDeadline);

  struct BodySlot {
    bool skipped_for_deadline = false;
    CollectorOutcome parsed;
  };
  std::vector<BodySlot> slots(html_bodies.size());
  std::atomic<size_t> next_task{0};
  std::atomic<bool> deadline_tripped{false};
  const auto worker_fn = [&] {
    for (;;) {
      const size_t idx = next_task.fetch_add(1, std::memory_order_relaxed);
      if (idx >= html_bodies.size()) break;
      if (deadline_tripped.load(std::memory_order_relaxed) ||
          (cancelled && cancelled()) ||
          std::chrono::system_clock::now() >= bodies_deadline) {
        deadline_tripped.store(true, std::memory_order_relaxed);
        slots[idx].skipped_for_deadline = true;
        continue;
      }
      const emailv1::BodyPart& body = html_bodies[idx];
      slots[idx].parsed =
          collect_markup_document(markup, "part-" + body.part_id() + ".html", "text/html",
                                  body.text(), bodies_deadline, cancelled);
    }
  };
  const size_t worker_count =
      std::min(html_bodies.size(), kMaxConcurrentBodyDials);
  std::vector<std::thread> workers;
  workers.reserve(worker_count - 1);
  for (size_t w = 1; w < worker_count; ++w) {
    workers.emplace_back(worker_fn);
  }
  worker_fn();
  for (auto& worker : workers) worker.join();

  for (size_t idx = 0; idx < html_bodies.size(); ++idx) {
    const emailv1::BodyPart& body = html_bodies[idx];
    BodySlot& slot = slots[idx];
    if (slot.skipped_for_deadline) {
      outcome->warnings.push_back("HTML body part " + body.part_id() +
                                  " and the ones after it were not parsed: the deadline "
                                  "passed or the call was cancelled");
      break;
    }
    for (auto& warning : slot.parsed.warnings) {
      outcome->warnings.push_back("HTML body part " + body.part_id() + ": " + warning);
    }
    if (!slot.parsed.success) {
      outcome->warnings.push_back("HTML body part " + body.part_id() +
                                  " could not be parsed by the markup collector: " +
                                  slot.parsed.error);
      continue;
    }
    strip_document_identity(&slot.parsed.document);
    const int insert_at = body_insertion_point(outcome->document);
    const int first_new = outcome->document.body().children_size();
    merge_documents(std::move(slot.parsed.document), &outcome->document);
    auto* children = outcome->document.mutable_body()->mutable_children();
    std::rotate(children->begin() + insert_at, children->begin() + first_new, children->end());
  }
}

}  // namespace

CollectorOutcome collect_email_document(const std::shared_ptr<grpc::Channel>& channel,
                                        const std::shared_ptr<grpc::Channel>& markup,
                                        const std::string& document_id,
                                        const std::string& filename,
                                        const std::string& content_type,
                                        const std::string& bytes,
                                        CollectorDeadline inbound_deadline,
                                        CollectorCancelled cancelled) {
  auto stub = emailv1::EmailParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  const CancelWatch watch(context, cancelled);
  auto stream = stub->ParseEmail(&context);

  emailv1::ParseEmailRequest request;
  emailv1::ParseEmailOptions* options = request.mutable_options();
  options->set_document_id(document_id);
  options->set_filename(filename);
  options->set_content_type(content_type);
  options->set_emit_document(true);
  // The email wire requires the final frame to declare itself: a half-close
  // without a complete-marked chunk is a truncated upload by contract, so
  // even an empty payload sends one frame.
  ConcurrentUpload upload(
      context, *stream, request, bytes, /*always_send_chunk=*/true,
      [&bytes](emailv1::ParseEmailRequest& frame, size_t offset, size_t length, bool last) {
        emailv1::EmailChunk* chunk = frame.mutable_chunk();
        chunk->set_data(bytes.data() + offset, length);
        chunk->set_complete(last);
      });
  // The fold maps text/plain bodies only and leaves HTML to the HTML
  // collector, so the HTML parts are kept off the stream for a message that
  // has no plain alternative.
  bool plain_body = false;
  std::vector<emailv1::BodyPart> html_bodies;
  CollectorOutcome outcome = drain_stream<emailv1::ParseEmailResponse>(
      "email", *stream, upload,
      [&plain_body, &html_bodies](emailv1::ParseEmailResponse& event,
                                  std::vector<std::string>& warnings) {
        if (event.has_body_part()) {
          if (event.body_part().media_type() == emailv1::BODY_MEDIA_TYPE_PLAIN) {
            plain_body = true;
          } else if (event.body_part().media_type() == emailv1::BODY_MEDIA_TYPE_HTML) {
            html_bodies.push_back(std::move(*event.mutable_body_part()));
          }
          return false;
        }
        if (!event.has_status()) return false;
        for (const auto& warning : event.status().warnings()) {
          warnings.push_back(warning);
        }
        return true;
      });
  if (!outcome.success || plain_body || html_bodies.empty()) return outcome;
  if (markup == nullptr) {
    outcome.warnings.push_back(
        "the HTML body was not folded: the markup collector is not configured "
        "(GRPARSE_MARKUP_TARGET), so the message carries no body text");
    return outcome;
  }
  fold_html_bodies(markup, std::move(html_bodies), inbound_deadline, cancelled, &outcome);
  return outcome;
}
}  // namespace grparse
