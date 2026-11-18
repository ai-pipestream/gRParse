#!/usr/bin/env bash
# Runs scripts/test/gpu-placement-cuda-test.sh against a fake docker, curl
# and nvidia-smi, so the receipt's own checks are tested where there is no
# GPU: a server that behaves passes, and one that comes up without a GPU,
# reports a model on ONNX Runtime's CUDA provider, has that provider's
# library loaded, builds engines on a cached boot, holds no GPU memory,
# counts a refusal or misreads the page fails with the matching message.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/bin" "$work/models"
: > "$work/models/ch_ppocr_mobile_v2.0_cls_infer.onnx"

# The fake server. A detached run boots it: every engine is built unless the
# cache volume already holds them. A foreground run is one of the receipt's
# startup-failure cases, the stream client or the model check.
cat > "$work/bin/docker" <<'FAKE'
#!/usr/bin/env bash
state=$FAKE_STATE
has() { local want=$1; shift; for argument in "$@"; do [ "$argument" = "$want" ] && return 0; done; return 1; }
matches() { local want=$1; shift; for argument in "$@"; do case "$argument" in $want) return 0 ;; esac; done; return 1; }
case "$1" in
  volume) exit 0 ;;
  port) echo "127.0.0.1:19464" ;;
  logs) cat "$state/log" ;;
  inspect)
    [ -f "$state/running" ] || exit 1
    case "$3" in *Pid*) echo 4242 ;; *) echo true ;; esac ;;
  exec)
    if has onnxruntime_providers_cuda "$@"; then [ -n "${FAKE_ORT_CUDA:-}" ]; exit $?; fi
    exit 0 ;;
  rm) rm -f "$state/running" ;;
  run)
    if has -d "$@"; then
      touch "$state/running"
      if matches '*:/var/cache/tensorrt' "$@" && [ -f "$state/cached" ] && [ -z "${FAKE_REBUILD:-}" ]; then
        word="loaded from /var/cache/tensorrt/x.engine"
      else
        word="built in 30.0 s (1 profile, 11.4.0.106 on a fake GPU)"
      fi
      matches '*:/var/cache/tensorrt' "$@" && touch "$state/cached"
      { for model in detection direction recognition; do echo "gRParse TensorRT: OCR $model engine $word"; done
        echo "gRParse listening on 0.0.0.0:50051"; } > "$state/log"
      exit 0
    fi
    if has grparse-stream-client "$@"; then
      echo "${FAKE_PAGE:-page=1 text_items=4 digital_items=0 ocr_items=4 labelled=2 tables=1 cells=20 pictures=1 picture_images=0 barcodes=0} ms=300"
      exit 0
    fi
    has grparse-tensorrt-check "$@" && exit "${FAKE_CHECK_EXIT:-0}"
    if [ -n "${FAKE_BOOTS_ANYWHERE:-}" ]; then echo "gRParse listening on 0.0.0.0:50051"; exit 0; fi
    if ! has --gpus "$@"; then
      echo "Startup failed: gRParse OCR detection: TensorRT (CUDA device 0) refused /models/det.onnx (no CUDA-capable device is detected); GPU placement is strict and this model will not run on ONNX Runtime or on CPU"
    elif has GRPARSE_CUDA_DEVICE=7 "$@"; then
      echo "Startup failed: TensorRT: CUDA device 7 does not exist (1 device visible)"
    elif matches 'GRPARSE_TENSORRT_CACHE_DIR=*' "$@"; then
      echo "Startup failed: GRPARSE_TENSORRT_CACHE_DIR=/models/engines is not a writable directory"
    else
      echo "Startup failed: TensorRT could not import the ONNX model: bad; GPU placement is strict and this model will not run on ONNX Runtime or on CPU"
    fi
    exit 1 ;;
  *) echo "fake docker: unexpected $*" >&2; exit 2 ;;
esac
FAKE
cat > "$work/bin/curl" <<'FAKE'
#!/usr/bin/env bash
sed "s/actual=\"TensorRT\"/actual=\"${FAKE_ACTUAL:-TensorRT}\"/" "$FAKE_GOLDEN"
echo "grparse_ort_ep_fallbacks_total ${FAKE_FALLBACKS:-0}"
FAKE
cat > "$work/bin/nvidia-smi" <<'FAKE'
#!/usr/bin/env bash
case "$*" in
  -L) echo "GPU 0: a fake GPU" ;;
  *compute-apps*) echo "4242, ${FAKE_GPU_MEMORY:-900}" ;;
esac
FAKE
chmod +x "$work/bin/docker" "$work/bin/curl" "$work/bin/nvidia-smi"

# run <label> [NAME=value ...]: one receipt run; returns its exit code.
run() {
  local label=$1; shift
  rm -rf "$work/state"; mkdir "$work/state"
  env PATH="$work/bin:$PATH" FAKE_STATE="$work/state" \
    FAKE_GOLDEN="$root/tests/data/gpu_placement/cuda.prom" GRPARSE_GPU_BOOT_SECONDS=20 "$@" \
    bash "$root/scripts/test/gpu-placement-cuda-test.sh" fake-image "$work/models" > "$work/$label.out" 2>&1
}
fail=0
expect_failure() {  # label, text the failure must carry, variables
  local label=$1 text=$2 code=0; shift 2
  run "$label" "$@" || code=$?
  if [ "$code" = 0 ]; then echo "FAIL: $label passed" >&2; fail=1; return; fi
  grep -qF "$text" "$work/$label.out" || { echo "FAIL: $label does not say '$text'" >&2; cat "$work/$label.out" >&2; fail=1; }
}

run good || { echo "FAIL: a well-behaved server failed" >&2; cat "$work/good.out" >&2; fail=1; }
grep -q "gpu-placement-cuda: ok" "$work/good.out" || { echo "FAIL: no ok line" >&2; fail=1; }
grep -q "boot cache, second: 0 engines built, 3 loaded from the cache" "$work/good.out" ||
  { echo "FAIL: the cached boot's engine counts are not reported" >&2; cat "$work/good.out" >&2; fail=1; }

expect_failure no-gpu-boots "no GPU in the container: startup exit 0, wanted a failure" FAKE_BOOTS_ANYWHERE=1
expect_failure ort-session "placement differs from" FAKE_ACTUAL=CUDA
expect_failure provider-loaded "ONNX Runtime's CUDA provider is loaded in the server" FAKE_ORT_CUDA=1
expect_failure cache-rebuilds "boot cache, second: 3 engines built, wanted 0" FAKE_REBUILD=1
expect_failure idle-gpu "the server holds no GPU memory" FAKE_GPU_MEMORY=0
expect_failure refusal "grparse_ort_ep_fallbacks_total is not 0" FAKE_FALLBACKS=1
expect_failure page "the page did not read as" FAKE_PAGE="page=1 text_items=4 digital_items=0 ocr_items=4 labelled=2 tables=0 cells=0 pictures=1 picture_images=0 barcodes=0"
expect_failure model-check "grparse-tensorrt-check failed" FAKE_CHECK_EXIT=1

[ "$fail" -eq 0 ] && echo "gpu-placement-cuda-selftest: ok"
exit "$fail"
