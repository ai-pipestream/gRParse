#include "grparse/document_parser_service.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/grpcpp.h>

#include "grparse/collector_coordinator.h"

namespace pipestream = ai::pipestream;

namespace grparse {

const char* collector_name(pipestream::parse::v1::Collector collector) {
  switch (collector) {
    case pipestream::parse::v1::COLLECTOR_GRPARSE_CV: return "grparse-cv";
    case pipestream::parse::v1::COLLECTOR_LIBREOFFICE: return "libreoffice";
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
    case pipestream::parse::v1::COLLECTOR_VLM: return "vlm-convert";
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
    case pipestream::parse::v1::COLLECTOR_CALAMINE: return targets_.calamine;
    default: return kNone;
  }
}

grpc::ChannelArguments collector_channel_arguments(const CollectorChannelOptions& options,
                                                   size_t channel_index) {
  grpc::ChannelArguments arguments;
  arguments.SetMaxReceiveMessageSize(kMaxMessageBytes);
  arguments.SetMaxSendMessageSize(kMaxMessageBytes);
  arguments.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, static_cast<int>(options.keepalive_time.count()));
  arguments.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,
                   static_cast<int>(options.keepalive_timeout.count()));
  // What actually enforces the keepalive timeout (see kPingTimeoutArgument).
  arguments.SetInt(kPingTimeoutArgument, static_cast<int>(options.keepalive_timeout.count()));
  // Pings only while a call is open: an idle pool is not this channel's to
  // keep warm, and a peer that refuses idle pings would GOAWAY it.
  arguments.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 0);
  // No cap on pings between data frames: a libreoffice or asr leg can go
  // minutes without a byte either way and must still be probed.
  arguments.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
  arguments.SetInt(GRPC_ARG_ENABLE_RETRIES, 0);
  arguments.SetInt(kChannelIndexArgument, static_cast<int>(channel_index));
  return arguments;
}

std::shared_ptr<grpc::Channel> CollectorEndpoints::next_channel(ChannelPool& pool,
                                                                const std::string& target,
                                                                size_t size) {
  if (pool.channels.empty()) {
    size = std::max<size_t>(1, size);
    pool.channels.reserve(size);
    for (size_t index = 0; index < size; ++index) {
      pool.channels.push_back(grpc::CreateCustomChannel(
          target, grpc::InsecureChannelCredentials(),
          collector_channel_arguments(targets_.channel_options, index)));
    }
  }
  auto channel = pool.channels[pool.next];
  pool.next = (pool.next + 1) % pool.channels.size();
  return channel;
}

std::shared_ptr<grpc::Channel> CollectorEndpoints::channel(
    pipestream::parse::v1::Collector id) {
  const std::string& where = target(id);
  if (where.empty()) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  return next_channel(channels_[id], where, targets_.channel_options.channels);
}

std::shared_ptr<grpc::Channel> CollectorEndpoints::enrich_channel() {
  if (!has_derender()) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  // One channel: enrich is a peer dialed once per document after the
  // merge, not a collector with a fan-out of concurrent legs.
  return next_channel(enrich_channel_, targets_.derender.target, 1);
}

std::shared_ptr<grpc::Channel> CollectorEndpoints::vlm_channel() {
  if (!has_vlm()) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  return next_channel(vlm_channel_, targets_.vlm.target, 1);
}

}  // namespace grparse
