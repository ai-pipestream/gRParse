#include "grparse/document_collectors.h"

#include <string>
#include <utility>
#include <vector>

#include "ai/pipestream/markup/v1/markup_service.grpc.pb.h"
#include "collector_support.h"

namespace docv1 = ai::pipestream::document::v1;
namespace markupv1 = ai::pipestream::markup::v1;

namespace grparse {

CollectorOutcome collect_markup_document(const std::shared_ptr<grpc::Channel>& channel,
                                         const std::string& filename,
                                         const std::string& content_type,
                                         const std::string& bytes,
                                         CollectorDeadline inbound_deadline) {
  auto stub = markupv1::MarkupParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  auto stream = stub->ParseMarkup(&context);

  markupv1::ParseMarkupRequest request;
  // The hint spares the collector a sniff and resolves what a sniff cannot:
  // Markdown and AsciiDoc have no reliable signature, but the filename
  // does. An unresolved hint stays MARKUP_FORMAT_UNSPECIFIED, which is the
  // wire's "sniff it".
  request.mutable_options()->set_format(markup_format_for(filename, content_type));
  request.mutable_options()->set_emit_document(true);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](markupv1::ParseMarkupRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.set_chunk(bytes.data() + offset, length);
                });
  CollectorOutcome outcome = drain_stream<markupv1::ParseMarkupResponse>(
      "markup", *stream,
      [](const markupv1::ParseMarkupResponse& event,
         std::vector<std::string>& warnings) {
        if (!event.has_status()) return false;
        for (const auto& warning : event.status().warnings()) {
          std::string text =
              markupv1::WarningCode_Name(warning.code()) + ": " + warning.message();
          if (warning.count() > 1) {
            text += " (x" + std::to_string(warning.count()) + ")";
          }
          warnings.push_back(std::move(text));
        }
        return true;
      });
  return outcome;
}

bool promote_source_title(docv1::Document* document) {
  if (document == nullptr || !document->has_source_meta()) return false;
  const std::string& title = document->source_meta().title();
  if (title.empty()) return false;
  for (const docv1::BaseTextItem& item : document->texts()) {
    if (item.has_title()) return false;
  }
  const std::string ref = "#/texts/" + std::to_string(document->texts_size());
  docv1::TextItemBase* base = document->add_texts()->mutable_title()->mutable_base();
  base->set_self_ref(ref);
  base->mutable_parent()->set_ref("#/body");
  base->set_label(docv1::DOC_ITEM_LABEL_TITLE);
  base->set_content_layer(docv1::CONTENT_LAYER_BODY);
  base->set_text(title);
  base->set_orig(title);
  docv1::CollectorSource* source = base->add_source()->mutable_collector();
  source->set_collector("grparse");
  source->set_model("source-meta-title");
  // The title leads the body: append, then rotate it to the front.
  auto* children = document->mutable_body()->mutable_children();
  children->Add()->set_ref(ref);
  for (int i = children->size() - 1; i > 0; i--) children->SwapElements(i, i - 1);
  return true;
}
}  // namespace grparse
