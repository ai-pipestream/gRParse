#include "grparse/document_parser_service.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "grparse/collector_coordinator.h"

namespace pipestream = ai::pipestream;

namespace grparse {

const char* collector_name(pipestream::parse::v1::Collector collector) {
  switch (collector) {
    case pipestream::parse::v1::COLLECTOR_GRPARSE_CV: return "grparse-cv";
    case pipestream::parse::v1::COLLECTOR_LIBREOFFICE: return "libreoffice";
    case pipestream::parse::v1::COLLECTOR_POI: return "poi";
    case pipestream::parse::v1::COLLECTOR_CALAMINE: return "calamine";
    case pipestream::parse::v1::COLLECTOR_ASR: return "asr";
    case pipestream::parse::v1::COLLECTOR_EMAIL: return "email";
    case pipestream::parse::v1::COLLECTOR_XML: return "xml";
    case pipestream::parse::v1::COLLECTOR_EBCDIC: return "ebcdic";
    case pipestream::parse::v1::COLLECTOR_EPUB: return "epub";
    case pipestream::parse::v1::COLLECTOR_MARKUP: return "markup";
    case pipestream::parse::v1::COLLECTOR_LOL_HTML: return "lol-html";
    case pipestream::parse::v1::COLLECTOR_FASTWARC: return "fastwarc";
    case pipestream::parse::v1::COLLECTOR_PDF: return "pdf";
    case pipestream::parse::v1::COLLECTOR_CONFLUENCE: return "confluence-storage";
    default: return "unspecified";
  }
}

const std::string& CollectorEndpoints::target(
    pipestream::parse::v1::Collector id) const {
  static const std::string kNone;
  switch (id) {
    case pipestream::parse::v1::COLLECTOR_LIBREOFFICE: return targets_.libreoffice;
    case pipestream::parse::v1::COLLECTOR_ASR: return targets_.asr;
    case pipestream::parse::v1::COLLECTOR_EMAIL: return targets_.email;
    case pipestream::parse::v1::COLLECTOR_XML: return targets_.xml;
    case pipestream::parse::v1::COLLECTOR_EBCDIC: return targets_.ebcdic;
    case pipestream::parse::v1::COLLECTOR_EPUB: return targets_.epub;
    case pipestream::parse::v1::COLLECTOR_MARKUP: return targets_.markup;
    case pipestream::parse::v1::COLLECTOR_LOL_HTML: return targets_.lol_html;
    case pipestream::parse::v1::COLLECTOR_FASTWARC: return targets_.fastwarc;
    case pipestream::parse::v1::COLLECTOR_PDF: return targets_.pdf;
    default: return kNone;
  }
}

grpc::ChannelArguments collector_channel_arguments() {
  grpc::ChannelArguments arguments;
  arguments.SetMaxReceiveMessageSize(kMaxMessageBytes);
  arguments.SetMaxSendMessageSize(kMaxMessageBytes);
  return arguments;
}

std::shared_ptr<grpc::Channel> CollectorEndpoints::channel(
    pipestream::parse::v1::Collector id) {
  const std::string& where = target(id);
  if (where.empty()) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  auto& channel = channels_[id];
  if (channel == nullptr) {
    channel = grpc::CreateCustomChannel(where, grpc::InsecureChannelCredentials(),
                                        collector_channel_arguments());
  }
  return channel;
}

std::shared_ptr<grpc::Channel> CollectorEndpoints::enrich_channel() {
  if (!has_derender()) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  if (enrich_channel_ == nullptr) {
    enrich_channel_ = grpc::CreateCustomChannel(targets_.derender.target,
                                                grpc::InsecureChannelCredentials(),
                                                collector_channel_arguments());
  }
  return enrich_channel_;
}

}  // namespace grparse
