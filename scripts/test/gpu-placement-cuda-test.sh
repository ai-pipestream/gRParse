#!/usr/bin/env bash
# GPU placement receipt for the CUDA image: proves every image model runs on
# native TensorRT where the golden says, that no ONNX Runtime CUDA session
# exists beside the engines, that the engine cache works and is off by
# default, and that a missing GPU, a missing device, a model TensorRT cannot
# import or an unwritable cache stops startup instead of running elsewhere.
#
#   scripts/test/gpu-placement-cuda-test.sh <cuda-image> <models-dir>
#
# Needs a host with an NVIDIA GPU and the container toolkit (`docker run
# --gpus all`); krick's RTX 4080 is the fleet's. Without one the test FAILS;
# it never skips, so a host that lost its GPU cannot pass.
#
# Checks, in order:
#   1. no GPU in the container: startup must fail with TensorRT's refusal
#      (no ONNX Runtime session and no CPU session is built instead);
#   2. GRPARSE_CUDA_DEVICE names a device that does not exist: must fail;
#   3. a model file TensorRT cannot import: must fail with the parser's
#      error and the strict-placement text;
#   4. GRPARSE_TENSORRT_CACHE_DIR on a read-only mount: must fail, naming
#      the variable;
#   5. a read-only root filesystem and no cache (the image's default): every
#      engine is built, none is loaded, and the server serves;
#   6. a cache volume, two boots: the first builds and stores every engine,
#      the second loads every one and builds none.
#   On each of the three good boots the grparse_ort_sessions series must
#   equal tests/data/gpu_placement/cuda.prom, the refusal counter must read
#   0, the server process must hold GPU memory, ONNX Runtime's CUDA provider
#   library must not be mapped into it, and tests/data/report_page.png must
#   read as it does on every other backend (its OCR items, layout labels,
#   table cells and pictures counted).
#   7. grparse-tensorrt-check in the image: every model file as a TensorRT
#      engine against the ONNX Runtime CPU reference on that page.
#
# GRPARSE_GPU_BOOT_SECONDS (default 1800) bounds one startup; a boot without
# a cache builds every engine first.
set -euo pipefail
image=${1:?cuda image}
models=${2:?models dir}
root=$(cd "$(dirname "$0")/../.." && pwd)
golden="$root/tests/data/gpu_placement/cuda.prom"
boot_seconds=${GRPARSE_GPU_BOOT_SECONDS:-1800}
work=$(mktemp -d)
server=grparse-gpu-placement-cuda-$$
cache=grparse-gpu-placement-cuda-cache-$$
cleanup() {
  if docker inspect "$server" >/dev/null 2>&1; then
    echo "== server log (tail)"
    docker logs "$server" 2>&1 | tail -40 || true
    docker rm -f "$server" >/dev/null 2>&1 || true
  fi
  docker volume rm "$cache" >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT
fail() { echo "gpu-placement-cuda: FAIL: $*" >&2; exit 1; }

command -v nvidia-smi >/dev/null 2>&1 || fail "no nvidia-smi on this host; this test needs the NVIDIA GPU"
nvidia-smi -L | grep -q '^GPU 0' || fail "nvidia-smi lists no GPU 0"
base_env=(-e GRPARSE_ORT_EP=cuda -e GRPARSE_PAGE_WORKERS=2 -e GRPARSE_METRICS_PORT=9464)
docker volume create "$cache" >/dev/null

# A startup that must fail: exits non-zero within the timeout, never
# listens, and its log carries the expected text.
expect_startup_failure() {
  local label=$1 text=$2; shift 2
  echo "== $label: startup must fail"
  set +e
  timeout 600 docker run --rm "${base_env[@]}" "$@" "$image" > "$work/fail.log" 2>&1
  local code=$?
  set -e
  grep -E "Startup failed|refused|GRPARSE_" "$work/fail.log" | head -3 || true
  [ "$code" != 0 ] && [ "$code" != 124 ] || fail "$label: startup exit $code, wanted a failure"
  grep -qF "$text" "$work/fail.log" || fail "$label: log lacks '$text'"
  ! grep -q "gRParse listening on" "$work/fail.log" || fail "$label: the server came up"
  ! grep -q "model placement: .* on CPU" "$work/fail.log" || fail "$label: a model was placed on CPU"
  echo "failed as required (exit $code)"
}

expect_startup_failure "no GPU in the container" "TensorRT (CUDA device 0) refused" \
  -v "$models:/models:ro"
expect_startup_failure "CUDA device 7" "CUDA device 7 does not exist" \
  --gpus all -v "$models:/models:ro" -e GRPARSE_CUDA_DEVICE=7
mkdir "$work/broken-models"
cp -r "$models"/. "$work/broken-models/"
printf 'not an ONNX model' > "$work/broken-models/ch_ppocr_mobile_v2.0_cls_infer.onnx"
chmod -R a+rX "$work/broken-models"
expect_startup_failure "a model TensorRT cannot import" "TensorRT could not import the ONNX model" \
  --gpus all -v "$work/broken-models:/models:ro"
grep -qF "will not run on ONNX Runtime or on CPU" "$work/fail.log" ||
  fail "the import failure lacks the strict-placement text"
expect_startup_failure "an unwritable engine cache" "GRPARSE_TENSORRT_CACHE_DIR=/models/engines is not a writable directory" \
  --gpus all -v "$models:/models:ro" -e GRPARSE_TENSORRT_CACHE_DIR=/models/engines

page_read="page=1 text_items=4 digital_items=0 ocr_items=4 labelled=2 tables=1 cells=20 pictures=1 picture_images=0 barcodes=0"
engine_lines() { docker logs "$server" 2>&1 | grep -c "gRParse TensorRT: .* engine $1" || true; }

# Boots the server with the given extra docker arguments and holds it to the
# golden; $2 and $3 are how many engines the log must say were built and
# loaded from the cache ("some" for at least one).
boot_and_check() {
  local pass=$1 want_built=$2 want_loaded=$3; shift 3
  echo "== boot $pass"
  local started=$SECONDS
  docker run -d --name "$server" --gpus all -p 127.0.0.1::9464 -v "$models:/models:ro" \
    "${base_env[@]}" "$@" "$image" >/dev/null
  local listening=false
  while [ $((SECONDS - started)) -lt "$boot_seconds" ]; do
    if docker logs "$server" 2>&1 | grep -q "gRParse listening on"; then listening=true; break; fi
    [ "$(docker inspect -f '{{.State.Running}}' "$server")" = true ] || break
    sleep 2
  done
  [ "$listening" = true ] || fail "boot $pass: server did not come up"
  echo "boot $pass: listening after $((SECONDS - started)) s"
  docker logs "$server" 2>&1 | grep "model placement" || true
  local built loaded
  built=$(engine_lines built)
  loaded=$(engine_lines "loaded from")
  echo "boot $pass: $built engines built, $loaded loaded from the cache"
  case $want_built in
    some) [ "$built" -gt 0 ] || fail "boot $pass: no engine was built" ;;
    *) [ "$built" = "$want_built" ] || fail "boot $pass: $built engines built, wanted $want_built" ;;
  esac
  case $want_loaded in
    some) [ "$loaded" -gt 0 ] || fail "boot $pass: no engine came from the cache" ;;
    *) [ "$loaded" = "$want_loaded" ] || fail "boot $pass: $loaded engines loaded, wanted $want_loaded" ;;
  esac

  local port
  port=$(docker port "$server" 9464/tcp | head -1 | sed 's/.*://')
  curl -sf "http://127.0.0.1:$port/metrics" > "$work/metrics-$pass.prom"
  grep '^grparse_ort_sessions{' "$work/metrics-$pass.prom" > "$work/placement-$pass.prom" || true
  diff -u "$golden" "$work/placement-$pass.prom" || fail "boot $pass: placement differs from $golden"
  grep -qx 'grparse_ort_ep_fallbacks_total 0' "$work/metrics-$pass.prom" ||
    fail "boot $pass: grparse_ort_ep_fallbacks_total is not 0"

  docker run --rm --network "container:$server" -v "$root/tests/data:/data:ro" \
    --entrypoint grparse-stream-client "$image" /data/report_page.png localhost:50051 \
    > "$work/convert-$pass.out" 2>&1 || { cat "$work/convert-$pass.out"; fail "boot $pass: conversion failed"; }
  # What the page reads as on every backend: four text items from OCR, two
  # of them labelled by layout, one table of twenty cells, one picture.
  grep -q "^$page_read " "$work/convert-$pass.out" ||
    { cat "$work/convert-$pass.out"; fail "boot $pass: the page did not read as '$page_read'"; }

  local pid memory
  pid=$(docker inspect -f '{{.State.Pid}}' "$server")
  memory=$(nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader,nounits |
    awk -F', ' -v pid="$pid" '$1 == pid { print $2 }')
  echo "boot $pass: server pid $pid holds ${memory:-0} MiB on the GPU"
  [ "${memory:-0}" -gt 0 ] || fail "boot $pass: the server holds no GPU memory"
  # No ONNX Runtime CUDA session can exist without its provider library.
  if docker exec "$server" grep -q onnxruntime_providers_cuda /proc/1/maps; then
    fail "boot $pass: ONNX Runtime's CUDA provider is loaded in the server"
  fi
  docker exec "$server" grep -q libnvinfer /proc/1/maps ||
    fail "boot $pass: TensorRT is not loaded in the server"
  docker rm -f "$server" >/dev/null
}

boot_and_check "read-only, no cache" some 0 --read-only
total=$(grep -c . "$golden")
boot_and_check "cache, first" some 0 -v "$cache:/var/cache/tensorrt" \
  -e GRPARSE_TENSORRT_CACHE_DIR=/var/cache/tensorrt
boot_and_check "cache, second" 0 some -v "$cache:/var/cache/tensorrt" \
  -e GRPARSE_TENSORRT_CACHE_DIR=/var/cache/tensorrt
cmp -s "$work/placement-cache, first.prom" "$work/placement-cache, second.prom" ||
  fail "placement changed between boots"
echo "placement: $total series equal to the golden on three boots"

echo "== grparse-tensorrt-check: every model against the CPU reference"
docker run --rm --gpus all -v "$models:/models:ro" -v "$root/tests/data:/data:ro" \
  -e GRPARSE_TEST_REQUIRE_GPU=1 -e GRPARSE_TEST_REQUIRE_MODELS=1 \
  -e GRPARSE_TENSORRT_CHECK_IMAGE=/data/report_page.png \
  --entrypoint grparse-tensorrt-check "$image" || fail "grparse-tensorrt-check failed"
echo "gpu-placement-cuda: ok"
