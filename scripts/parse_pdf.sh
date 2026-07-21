#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 PDF_PATH [HOST:PORT]" >&2
  exit 64
fi

pdf_path=$1
endpoint=${2:-localhost:50051}
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

if [[ ! -f "$pdf_path" ]]; then
  echo "PDF not found: $pdf_path" >&2
  exit 66
fi

client=${GRPARSE_STREAM_CLIENT:-}
if [[ -z "$client" ]]; then
  for candidate in \
    "$project_root/build/grparse-stream-client" \
    "$project_root/cmake-build-release/grparse-stream-client" \
    /usr/local/bin/grparse-stream-client; do
    if [[ -x "$candidate" ]]; then
      client=$candidate
      break
    fi
  done
fi
if [[ -z "$client" || ! -x "$client" ]]; then
  echo "grparse-stream-client was not found; build it or set GRPARSE_STREAM_CLIENT" >&2
  exit 69
fi

exec "$client" "$pdf_path" "$endpoint"
