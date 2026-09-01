#include "grparse/document_collectors.h"

#include <string>
#include <utility>
#include <vector>

#include "ai/pipestream/xml/v1/xml_service.grpc.pb.h"
#include "collector_support.h"

namespace xmlv1 = ai::pipestream::xml::v1;

namespace grparse {

CollectorOutcome collect_xml_document(const std::shared_ptr<grpc::Channel>& channel,
                                      const std::string& bytes,
                                      CollectorDeadline inbound_deadline) {
  auto stub = xmlv1::XmlParseService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(capped_collector_deadline(inbound_deadline, kDeadline));
  auto stream = stub->ParseXml(&context);

  xmlv1::ParseXmlRequest request;
  request.mutable_options()->set_emit_document(true);
  upload_stream(*stream, request, bytes, /*always_send_chunk=*/false,
                [&bytes](xmlv1::ParseXmlRequest& frame, size_t offset,
                         size_t length, bool /*last*/) {
                  frame.set_chunk(bytes.data() + offset, length);
                });
  return drain_stream<xmlv1::ParseXmlResponse>(
      "xml", *stream,
      [](const xmlv1::ParseXmlResponse& event, std::vector<std::string>& warnings) {
        if (!event.has_status()) return false;
        for (const auto& warning : event.status().warnings()) {
          std::string text =
              xmlv1::WarningCode_Name(warning.code()) + ": " + warning.message();
          if (warning.count() > 1) {
            text += " (x" + std::to_string(warning.count()) + ")";
          }
          warnings.push_back(std::move(text));
        }
        return true;
      });
}
}  // namespace grparse
