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
#   3. non-root: the image runs as the numeric user 65532 (every image is
#      minimal-base compatible; a root default is a regression).
#
# Full check (--full, needs models/ populated next to this repo by
# scripts/fetch-models.sh, or SMOKE_MODELS_DIR pointing at a populated
# directory): boots the server on the CPU provider and streams a fixture
# through the bundled client, asserting a page event and the terminal
# complete event.
#
# Nothing here needs a shell inside the image: the closure check asks the
# dynamic loader itself (LD_TRACE_LOADED_OBJECTS is what ldd does under the
# hood), so a hardened runtime base without /bin/sh or ldd passes the same
# gate as a full distribution base.
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
for binary in /usr/local/bin/grparse-server /usr/local/bin/grparse-stream-client; do
  trace=$(docker run --rm -e LD_TRACE_LOADED_OBJECTS=1 --entrypoint "$binary" "$image" 2>&1 || true)
  if ! grep -q '=>' <<<"$trace"; then
    echo "the loader printed no dependency list for $binary in $image:" >&2
    echo "$trace" >&2
    exit 1
  fi
  if grep -q "not found" <<<"$trace"; then
    echo "unresolved shared libraries for $binary in $image:" >&2
    grep "not found" <<<"$trace" >&2
    exit 1
  fi
done

echo "== smoke: image runs as the non-root user 65532"
image_user=$(docker inspect --format '{{.Config.User}}' "$image")
if [[ "$image_user" != "65532:65532" ]]; then
  echo "expected USER 65532:65532, image has '${image_user:-root}'" >&2
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
  models_dir=${SMOKE_MODELS_DIR:-$project_root/models}
  docker run -d --rm --name "$container" -e GRPARSE_ORT_EP=cpu \
    -v "$models_dir:/models:ro" "$image" >/dev/null
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
