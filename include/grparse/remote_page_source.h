#pragma once

#include <memory>
#include <optional>
#include <string>

#include <grpcpp/grpcpp.h>

#include "grparse/in_memory_document.h"

namespace grparse {

struct CollectorChannelOptions;

// The wire a PDF backend source speaks to its backend. kCurrent is the
// ParseStream session: one chunked upload, then every page's text and
// raster asked for on the same stream. kLegacyFallback means the backend
// answered ParseStream UNIMPLEMENTED (it predates the RPC) and the source
// uses Probe/Parse/Render; kLegacyByConfig means
// GRPARSE_PDF_BACKEND_STREAM=off chose those calls.
enum class PdfBackendProtocol { kCurrent, kLegacyFallback, kLegacyByConfig };

// The wire a source from open_remote_pdf_document speaks, settled by its
// opening call (kCurrent before it on a source that will try ParseStream);
// nullopt for any other source.
std::optional<PdfBackendProtocol> pdf_backend_protocol(const PageSource& source);

// The PDF backend target from GRPARSE_PDF_BACKEND: a gRPC target
// ("host:port"), or a comma list of them, for a PdfBackendService
// (ai.protomolt.parse.pdf.v1) such as grpc-pdfium. Unset or empty means no
// backend is configured (nullopt). Surrounding whitespace is trimmed; a
// whitespace-only value is a config error.
std::optional<std::string> remote_pdf_backend_target();

// Returns the shared multiplexed gRPC channel for a PdfBackendService target,
// configured with the 520 MiB message size ceiling.
std::shared_ptr<grpc::Channel> pdf_backend_channel(const std::string& target);

// The keepalive timings every PdfBackendService channel made after this call
// is created with (the collector channels' options; the pool size does not
// apply, each target has one channel). main sets it once at startup, before
// the first channel; until then the defaults apply.
void set_pdf_backend_channel_options(const CollectorChannelOptions& options);

// Opens a PageSource whose pages come from a PdfBackendService. The source
// opens one ParseStream per document: the bytes upload in chunks while the
// open's header (load verdict, page inventory) is awaited, and every page's
// text (ParsePages) and raster (RenderPages, at render_dpi) is asked for on
// that stream. Opening happens before this returns (SourceOpening::kNow) or
// on the source's first use (kOnFirstUse); typed load failures raise
// InvalidDocument. A backend that answers ParseStream UNIMPLEMENTED gets the
// unary calls instead, recorded in pdf_backend_protocol(): loading verified
// through Probe, text pages as per-page Parse streams, rasters through
// Render. On that path the document is content-addressed: the client hashes the bytes once and
// addresses them by sha256 on every call, uploading the bytes exactly once
// per backend when a cache miss verdict (LOAD_STATUS_BYTES_REQUIRED) asks
// for them. GRPARSE_PDF_BACKEND_HANDSHAKE=off restores the old behavior of
// sending the bytes with every call. `precomputed_sha256`, when non-empty,
// supplies the hex SHA-256 digest of `*bytes` so multi-backend callers hash
// the document once instead of once per leg.
std::shared_ptr<PageSource> open_remote_pdf_document(
    std::shared_ptr<const std::string> bytes, const std::string& target,
    double render_dpi, SourceOpening opening = SourceOpening::kNow,
    std::string precomputed_sha256 = {});

}  // namespace grparse
