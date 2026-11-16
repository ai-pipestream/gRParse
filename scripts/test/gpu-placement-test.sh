#!/usr/bin/env bash
# GPU placement receipt for the Intel image: proves every model runs where the
# golden says, that the GPU does the work, and that a missing or vague device
# stops startup instead of running on CPU.
#
#   scripts/test/gpu-placement-test.sh <openvino-image> <models-dir>
#
# Needs a host with an Intel GPU render node (/dev/dri/renderD*); krick-1's
# Arc B70 is the fleet's. GRPARSE_RENDER_GID is the host gid of that node
# (default: read with stat). Without a render node the test FAILS; it never
# skips, so a host that lost its GPU cannot pass.
#
# Checks, in order:
#   1. no /dev/dri in the container: startup must fail with OpenVINO's
#      "Device GPU is not available" (no CPU session is built);
#   2. GRPARSE_OPENVINO_DEVICE=AUTO:GPU,CPU: startup must fail (device lists
#      let OpenVINO move work to CPU on its own);
#   3. with the GPU, two boots in a row: the grparse_ort_sessions series must
#      equal tests/data/gpu_placement/openvino.prom on both, the refusal
#      counter must read 0, and converting tests/data/report_page.png must
#      advance the server's own xe compute-engine cycles (drm-cycles-ccs in
#      /proc/1/fdinfo), so a model that silently left the GPU shows up as
#      either a placement diff or an idle engine.
set -euo pipefail
image=${1:?openvino image}
models=${2:?models dir}
root=$(cd "$(dirname "$0")/../.." && pwd)
golden="$root/tests/data/gpu_placement/openvino.prom"
work=$(mktemp -d)
server=grparse-gpu-placement-$$
cleanup() {
  if docker inspect "$server" >/dev/null 2>&1; then
    echo "== server log (tail)"
    docker logs "$server" 2>&1 | tail -40 || true
    docker rm -f "$server" >/dev/null 2>&1 || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT
fail() { echo "gpu-placement: FAIL: $*" >&2; exit 1; }

render=$(ls /dev/dri/renderD* 2>/dev/null | head -1 || true)
[ -n "$render" ] || fail "no /dev/dri/renderD* on this host; this test needs the Intel GPU"
gid=${GRPARSE_RENDER_GID:-$(stat -c %g "$render")}
gpu_args=(--device /dev/dri:/dev/dri --group-add "$gid")
base_env=(-e GRPARSE_ORT_EP=openvino -e GRPARSE_PAGE_WORKERS=2 -e GRPARSE_METRICS_PORT=9464
          -e GRPARSE_OPENVINO_CACHE_DIR=/var/cache/openvino)
cache=grparse-gpu-placement-cache
docker volume create "$cache" >/dev/null

# A startup that must fail: exits non-zero within the timeout, and its log
# carries the expected text.
expect_startup_failure() {
  local label=$1 text=$2; shift 2
  echo "== $label: startup must fail"
  set +e
  timeout 300 docker run --rm -v "$models:/models:ro" -v "$cache:/var/cache/openvino" \
    "${base_env[@]}" "$@" "$image" > "$work/fail.log" 2>&1
  local code=$?
  set -e
  grep -E "Startup failed|not available|GRPARSE_OPENVINO_DEVICE" "$work/fail.log" | head -3 || true
  [ "$code" != 0 ] && [ "$code" != 124 ] || fail "$label: startup exit $code, wanted a failure"
  grep -qF "$text" "$work/fail.log" || fail "$label: log lacks '$text'"
  echo "failed as required (exit $code)"
}

expect_startup_failure "no GPU device" "Device GPU is not available"
expect_startup_failure "AUTO device list" "AUTO, HETERO, MULTI and BATCH device lists" \
  "${gpu_args[@]}" -e GRPARSE_OPENVINO_DEVICE=AUTO:GPU,CPU

ccs_cycles() {
  docker exec "$server" sh -c \
    'total=0; for f in /proc/1/fdinfo/*; do
       v=$(grep -s "^drm-cycles-ccs:" "$f" | awk "{print \$2}");
       [ -n "$v" ] && total=$((total + v));
     done; echo $total'
}

boot_and_check() {
  local pass=$1
  echo "== boot $pass with the GPU"
  docker run -d --name "$server" -p 127.0.0.1::9464 -v "$models:/models:ro" \
    -v "$cache:/var/cache/openvino" "${base_env[@]}" "${gpu_args[@]}" "$image" >/dev/null
  local listening=false
  for _ in $(seq 1 300); do
    if docker logs "$server" 2>&1 | grep -q "gRParse listening on"; then listening=true; break; fi
    [ "$(docker inspect -f '{{.State.Running}}' "$server")" = true ] || break
    sleep 1
  done
  [ "$listening" = true ] || fail "boot $pass: server did not come up"
  docker logs "$server" 2>&1 | grep "model placement" || true
  local port
  port=$(docker port "$server" 9464/tcp | head -1 | sed 's/.*://')
  curl -sf "http://127.0.0.1:$port/metrics" > "$work/metrics-$pass.prom"
  grep '^grparse_ort_sessions{' "$work/metrics-$pass.prom" > "$work/placement-$pass.prom" || true
  if ! diff -u "$golden" "$work/placement-$pass.prom"; then
    fail "boot $pass: placement differs from $golden"
  fi
  grep -qx 'grparse_ort_ep_fallbacks_total 0' "$work/metrics-$pass.prom" ||
    fail "boot $pass: grparse_ort_ep_fallbacks_total is not 0"

  local before after
  before=$(ccs_cycles)
  docker run --rm --network "container:$server" -v "$root/tests/data:/data:ro" \
    --entrypoint grparse-stream-client "$image" /data/report_page.png localhost:50051 \
    > "$work/convert-$pass.out" 2>&1 || { cat "$work/convert-$pass.out"; fail "boot $pass: conversion failed"; }
  after=$(ccs_cycles)
  echo "boot $pass: compute-engine cycles $before -> $after"
  [ "$after" -gt "$before" ] || fail "boot $pass: the GPU compute engine did no work for this server"
  docker rm -f "$server" >/dev/null
}

boot_and_check 1
boot_and_check 2
cmp -s "$work/placement-1.prom" "$work/placement-2.prom" || fail "placement changed between boots"
echo "gpu-placement: ok"
