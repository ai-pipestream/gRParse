#!/usr/bin/env bash
# Boot-proofs a gRParse image: a green build is not "done" until the artifact
# actually starts.  The ZXing BUILD_SHARED_LIBS incident (2026-08) produced
# weeks of green builds whose server could not load its own libraries; these
# checks make that class of regression fail in CI and before any publish.
#
# Hermetic checks (no GPU, no model files — safe for CI and publish):
#   1. closure: every shared library both binaries link resolves in the image
#   2. boot-to-main: with no models mounted the server must fail with its OWN
#      "Required OCR model is missing" message, proving the loader, static
#      initialization, and configuration parsing all ran — not a loader error.
#
# Full check (--full, needs models/ populated next to this repo): boots the
# server on the CPU provider and streams a fixture through the bundled
# client, asserting a page event and the terminal complete event.
set -euo pipefail

usage() {
  echo "Usage: $0 IMAGE [--full]" >&2
  exit 64
}
[[ $# -ge 1 && $# -le 2 ]] || usage
image=$1
mode=${2:-}
[[ -z "$mode" || "$mode" == "--full" ]] || usage
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

echo "== smoke: library closure of the shipped binaries"
unresolved=$(docker run --rm --entrypoint /bin/sh "$image" -c \
  'ldd /usr/local/bin/grparse-server /usr/local/bin/grparse-stream-client 2>&1 | grep "not found" || true')
if [[ -n "$unresolved" ]]; then
  echo "unresolved shared libraries in $image:" >&2
  echo "$unresolved" >&2
  exit 1
fi

echo "== smoke: server reaches main (expects its own model-missing failure)"
boot_output=$(docker run --rm -e GRPARSE_ORT_EP=cpu "$image" 2>&1 || true)
echo "$boot_output"
if grep -q "error while loading shared libraries" <<<"$boot_output"; then
  echo "the loader failed before main ran" >&2
  exit 1
fi
if ! grep -q "Required OCR model is missing" <<<"$boot_output"; then
  echo "expected the server's own startup failure for absent models" >&2
  exit 1
fi

if [[ "$mode" == "--full" ]]; then
  echo "== smoke: CPU-provider boot and fixture stream"
  container="grparse-smoke-$$"
  docker run -d --rm --name "$container" -e GRPARSE_ORT_EP=cpu \
    -v "$project_root/models:/models:ro" "$image" >/dev/null
  trap 'docker rm -f "$container" >/dev/null 2>&1 || true' EXIT
  for _ in $(seq 1 60); do
    docker logs "$container" 2>&1 | grep -q "listening on" && break
    sleep 1
  done
  if ! docker logs "$container" 2>&1 | grep -q "listening on"; then
    echo "server did not reach listening; logs:" >&2
    docker logs "$container" >&2 || true
    exit 1
  fi
  stream_output=$(docker run --rm --network "container:$container" \
    -v "$project_root/tests/data/report_page.png:/input/report_page.png:ro" \
    --entrypoint /usr/local/bin/grparse-stream-client \
    "$image" /input/report_page.png localhost:50051)
  echo "$stream_output"
  grep -q "^page=1 " <<<"$stream_output" || { echo "expected a page event" >&2; exit 1; }
  grep -q "^complete " <<<"$stream_output" || { echo "expected the complete event" >&2; exit 1; }
fi

echo "smoke-test: OK ($image)"
