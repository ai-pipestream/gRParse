#pragma once

#include <memory>
#include <optional>
#include <string>

#include "grparse/in_memory_document.h"

namespace grparse {

// The PDF backend target from GRPARSE_PDF_BACKEND. Unset, empty, or the
// literal "inprocess" keep the in-process poppler path; anything else is a
// gRPC target ("host:port") for a PdfBackendService
// (ai.protomolt.parse.pdf.v1) such as grpc-pdfium.
std::optional<std::string> remote_pdf_backend_target();

// Opens a PageSource whose pages come from a PdfBackendService instead of
// the in-process poppler path. Loading is verified up front through Probe
// (typed load failures raise InvalidDocument); text pages arrive as
// per-page Parse streams and rasters through Render at render_dpi. The
// document is content-addressed: the client hashes the bytes once and
// addresses them by sha256 on every call, uploading the bytes exactly once
// per backend when a cache miss verdict (LOAD_STATUS_BYTES_REQUIRED) asks
// for them. GRPARSE_PDF_BACKEND_HANDSHAKE=off restores the old behavior of
// sending the bytes with every call.
std::shared_ptr<PageSource> open_remote_pdf_document(
    std::shared_ptr<const std::string> bytes, const std::string& target,
    double render_dpi);

}  // namespace grparse
