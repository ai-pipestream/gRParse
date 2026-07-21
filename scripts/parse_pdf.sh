#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 PDF_PATH [HOST:PORT]" >&2
  exit 64
fi

pdf_path=$1
endpoint=${2:-localhost:50051}
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
proto_root=$(mktemp -d /tmp/grparse-proto.XXXXXX)
encoded_pdf=$(mktemp /tmp/grparse-pdf.XXXXXX)

cleanup() {
  rm -rf "$proto_root" "$encoded_pdf"
}
trap cleanup EXIT

if [[ ! -f "$pdf_path" ]]; then
  echo "PDF not found: $pdf_path" >&2
  exit 66
fi
if ! command -v grpcurl >/dev/null; then
  echo "grpcurl is required to run this client" >&2
  exit 69
fi

mkdir -p "$proto_root/ai/docling/core/v1" "$proto_root/ai/docling/serve/v1"
cp "$project_root/docling.proto" "$proto_root/ai/docling/core/v1/docling_document.proto"
cp "$project_root/docling_serve_types.proto" "$proto_root/ai/docling/serve/v1/docling_serve_types.proto"
cp "$project_root/docling_serve.proto" "$proto_root/ai/docling/serve/v1/docling_serve.proto"

base64 -w0 "$pdf_path" > "$encoded_pdf"
jq -n --rawfile contents "$encoded_pdf" --arg filename "$(basename "$pdf_path")" \
  '{request: {sources: [{file: {base64String: $contents, filename: $filename}}], options: {toFormats: ["OUTPUT_FORMAT_TEXT"]}}}' \
  | grpcurl -plaintext \
      -import-path "$proto_root" \
      -proto ai/docling/core/v1/docling_document.proto \
      -proto ai/docling/serve/v1/docling_serve_types.proto \
      -proto ai/docling/serve/v1/docling_serve.proto \
      -d @ "$endpoint" ai.docling.serve.v1.DoclingServeService/ConvertSource
