#pragma once

// How an embedding failure leaves the process: a status for the client that
// says only that the batch failed, and the reason on the server's side.
//
// Internal to src: the engine and the chunk embedder share it.

#include <string_view>

#include <grpcpp/support/status.h>

#include "grparse/prometheus_metrics.h"

namespace grparse::embedding::detail {

// The INTERNAL status a failed embedding batch answers with, carrying
// `client_message` and nothing of `detail`. A vendor runtime's error text
// can quote the text being embedded or a path on this host, so it never
// reaches the caller; it is written to stderr instead, on one line that
// names `backend` and `stage`, and the failure is counted as
// grparse_embedding_failures_total{backend,stage}. Without both, every
// failure looks the same from outside: a lost device and a bad shape answer
// with the same words.
//
// `detail` is logged with its control characters turned into spaces and cut
// to a bounded length, so one failure is one line whatever the runtime
// wrote.
grpc::Status report_failure(std::string_view backend, EmbeddingFailureStage stage,
                            std::string_view detail, const char* client_message);

}  // namespace grparse::embedding::detail
