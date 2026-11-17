#!/usr/bin/env bash
# Hailo-10H placement receipt: proves every model runs where the golden says,
# that the chip does the work, and that a missing device or a missing HEF
# stops startup instead of running on CPU.
#
#   scripts/test/hailo-placement-test.sh <image-or-rootfs> <models-dir>
#
# <image-or-rootfs> is the image built from Dockerfile.cpu with
# GRPARSE_HAILORT_VERSION, or a directory holding that image's root
# filesystem (docker export), which runs under bubblewrap on a host with no
# Docker daemon. <models-dir> holds the ONNX files, the OCR dictionary and
# hailo10h/ with the HEFs.
#
# Needs a host with a Hailo-10H (/dev/hailo0); pi5ai2p is the fleet's.
# Without the device the test FAILS; it never skips, so a host that lost its
# chip cannot pass.
#
# Checks, in order:
#   1. no /dev/hailo0 in the container: startup must fail saying the device
#      was not found or did not open (no CPU session is built in its place);
#   2. an empty HEF directory: startup must fail naming the missing HEF;
#   3. with the device, two boots in a row with the anchor-free layout
#      detector, from a models directory that holds the HEFs, the OCR
#      dictionary and the table model and nothing else: no ONNX file of the
#      OCR, layout or figure models is in the container, so nothing but the
#      chip can have read the page. The grparse_ort_sessions series must
#      equal tests/data/gpu_placement/hailo.prom on both boots, the refusal
#      counter must read 0, and converting tests/mnt/data/report_page.png must
#      return OCR text and advance grparse_hailo_frames_total for the
#      detector, the recognizer and the layout model (a frame is counted
#      only when HailoRT reports it complete from the device);
#   4. one boot with the query layout detector: the series must equal
#      tests/data/gpu_placement/hailo-query-layout.prom, where layout is a
#      CPU pin with its reason.
set -euo pipefail
target=${1:?image or rootfs directory}
models=$(cd "${2:?models dir}" && pwd)
root=$(cd "$(dirname "$0")/../.." && pwd)
golden_dir="$root/tests/data/gpu_placement"
work=$(mktemp -d)
name=grparse-hailo-placement-$$
server_pid=
metrics_port=${GRPARSE_TEST_METRICS_PORT:-19464}
grpc_port=${GRPARSE_TEST_GRPC_PORT:-15051}
fail() { echo "hailo-placement: FAIL: $*" >&2; exit 1; }

[ -c /dev/hailo0 ] || fail "no /dev/hailo0 on this host; this test needs the Hailo-10H"
if [ -d "$target" ]; then
  mode=rootfs
  command -v bwrap >/dev/null || fail "a rootfs target needs bubblewrap (bwrap)"
else
  mode=docker
fi

# Every process below $1, deepest last.
descendants() {
  local child
  for child in $(ps -o pid= --ppid "$1" 2>/dev/null); do
    echo "$child"
    descendants "$child"
  done
}

# Stops the server with SIGTERM and waits for it to exit, so HailoRT closes
# the chip itself: a killed server leaves the chip held for the next boot.
stop_server() {
  if [ "$mode" = docker ]; then
    docker stop -t 30 "$name" >/dev/null 2>&1 || true
    docker rm -f "$name" >/dev/null 2>&1 || true
  elif [ -n "$server_pid" ]; then
    local pids pid
    pids=$(descendants "$server_pid")
    for pid in $pids; do
      [ "$(ps -o comm= -p "$pid" 2>/dev/null)" = grparse-server ] && kill -TERM "$pid" 2>/dev/null
    done
    for _ in $(seq 1 30); do
      kill -0 "$server_pid" 2>/dev/null || break
      sleep 1
    done
    kill "$server_pid" $pids 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    server_pid=
  fi
}
cleanup() {
  if [ -s "$work/server.log" ]; then
    echo "== server log (tail)"
    tail -30 "$work/server.log" || true
  fi
  stop_server
  rm -rf "$work"
}
trap cleanup EXIT

# run_in <device|nodevice> <hef-dir-in-container> <command...>: one process in
# the image, in the foreground, stdout and stderr to the caller.
run_in() {
  local device=$1 hefs=$2; shift 2
  local env=(GRPARSE_ORT_EP=hailo GRPARSE_PAGE_WORKERS=2 "GRPARSE_METRICS_PORT=$metrics_port"
             "GRPARSE_LISTEN_ADDRESS=127.0.0.1:$grpc_port" GRPARSE_MODELS_DIR=/mnt/models
             "GRPARSE_HAILO_HEF_DIR=$hefs" "GRPARSE_LAYOUT_MODEL=${LAYOUT_MODEL:-picodet}"
             LD_LIBRARY_PATH=/usr/local/lib HAILORT_LOGGER_PATH=NONE)
  if [ "$mode" = docker ]; then
    local args=(--rm --name "$name" --network host -v "$models:/mnt/models:ro"
                -v "$root/tests/data:/mnt/data:ro" -v "$work/empty:/mnt/empty-hefs:ro")
    [ "$device" = device ] && args+=(--device /dev/hailo0)
    for pair in "${env[@]}"; do args+=(-e "$pair"); done
    docker run "${args[@]}" --entrypoint "$1" "$target" "${@:2}"
  else
    # /sys as a container has it: HailoRT finds the device there and then
    # opens the node, which only the device runs are given.
    # The mounts go under /mnt, which every base image has: a read-only root
    # cannot grow new mount points.
    local args=(--ro-bind "$target" / --proc /proc --tmpfs /tmp --dev /dev --ro-bind /sys /sys
                --tmpfs /mnt --ro-bind "$models" /mnt/models --ro-bind "$root/tests/data" /mnt/data
                --ro-bind "$work/empty" /mnt/empty-hefs --unshare-user --share-net --die-with-parent)
    [ "$device" = device ] && args+=(--dev-bind /dev/hailo0 /dev/hailo0)
    for pair in "${env[@]}"; do args+=(--setenv "${pair%%=*}" "${pair#*=}"); done
    bwrap "${args[@]}" "$@"
  fi
}
mkdir -p "$work/empty"

# The models directory the device boots run from: everything the chip needs
# and the one model pinned to CPU. No OCR, layout or figure ONNX file.
all_models=$models
models="$work/models-device"
mkdir -p "$models"
cp "$all_models/ppocr_keys_v1.txt" "$all_models/slanet_plus.onnx" "$models/"
cp -r "$all_models/hailo10h" "$models/hailo10h"

# A startup that must fail: exits non-zero within the timeout, and its log
# carries the expected text.
expect_startup_failure() {
  local label=$1 text=$2 device=$3 hefs=$4
  echo "== $label: startup must fail"
  run_in "$device" "$hefs" grparse-server > "$work/fail.log" 2>&1 &
  local pid=$! code=124
  for _ in $(seq 1 120); do
    if ! kill -0 "$pid" 2>/dev/null; then
      set +e
      wait "$pid"
      code=$?
      set -e
      break
    fi
    sleep 1
  done
  if [ "$code" = 124 ]; then
    kill "$pid" 2>/dev/null || true
    [ "$mode" = docker ] && docker rm -f "$name" >/dev/null 2>&1
  fi
  grep -E "Startup failed|Hailo|HEF" "$work/fail.log" | head -3 || true
  [ "$code" != 0 ] && [ "$code" != 124 ] || fail "$label: startup exit $code, wanted a failure"
  grep -qE "$text" "$work/fail.log" || fail "$label: log lacks '$text'"
  ! grep -q "gRParse listening on" "$work/fail.log" || fail "$label: the server came up"
  echo "failed as required (exit $code)"
}

expect_startup_failure "no Hailo device" \
  "Startup failed: GRPARSE_ORT_EP=hailo: (no Hailo device|Hailo device [0-9a-f:.]+ did not open)" \
  nodevice /mnt/models/hailo10h
expect_startup_failure "no HEFs" "the HEF is missing" device /mnt/empty-hefs

metric() { awk -v key="$1" '$1 == key {print $2}' "$2" | head -1; }

boot() {
  local pass=$1 golden=$2
  echo "== boot $pass with the device (layout ${LAYOUT_MODEL:-picodet})"
  : > "$work/server.log"
  if [ "$mode" = docker ]; then
    (run_in device /mnt/models/hailo10h grparse-server > "$work/server.log" 2>&1 &)
  else
    run_in device /mnt/models/hailo10h grparse-server > "$work/server.log" 2>&1 &
    server_pid=$!
  fi
  local listening=false
  for _ in $(seq 1 300); do
    if grep -q "gRParse listening on" "$work/server.log"; then listening=true; break; fi
    if grep -q "Startup failed" "$work/server.log"; then break; fi
    sleep 1
  done
  [ "$listening" = true ] || fail "boot $pass: server did not come up"
  grep "model placement" "$work/server.log" || true
  curl -sf "http://127.0.0.1:$metrics_port/metrics" > "$work/metrics-$pass.prom"
  grep '^grparse_ort_sessions{' "$work/metrics-$pass.prom" > "$work/placement-$pass.prom" || true
  diff -u "$golden" "$work/placement-$pass.prom" || fail "boot $pass: placement differs from $golden"
  grep -qx 'grparse_ort_ep_fallbacks_total 0' "$work/metrics-$pass.prom" ||
    fail "boot $pass: grparse_ort_ep_fallbacks_total is not 0"
}

convert_and_check() {
  local pass=$1; shift
  name="$name-client" run_in nodevice /mnt/models/hailo10h grparse-stream-client /mnt/data/report_page.png \
    "127.0.0.1:$grpc_port" > "$work/convert-$pass.out" 2>&1 ||
    { cat "$work/convert-$pass.out"; fail "boot $pass: conversion failed"; }
  grep -E '^page=' "$work/convert-$pass.out"
  grep -Eq 'ocr_items=[1-9]' "$work/convert-$pass.out" || fail "boot $pass: the page came back with no OCR text"
  curl -sf "http://127.0.0.1:$metrics_port/metrics" > "$work/metrics-$pass-after.prom"
  for model in "$@"; do
    local key="grparse_hailo_frames_total{model=\"$model\"}"
    local before after
    before=$(grep -F "$key" "$work/metrics-$pass.prom" | awk '{print $NF}')
    after=$(grep -F "$key" "$work/metrics-$pass-after.prom" | awk '{print $NF}')
    echo "boot $pass: $model frames on the chip ${before:-none} -> ${after:-none}"
    [ -n "$before" ] && [ -n "$after" ] && [ "$after" -gt "$before" ] ||
      fail "boot $pass: $model ran no frame on the Hailo device"
  done
  ! grep '^grparse_hailo_model_reloads_total' "$work/metrics-$pass-after.prom" | grep -qv ' 0$' ||
    fail "boot $pass: the chip aborted a model during the conversion"
}

for pass in 1 2; do
  boot "$pass" "$golden_dir/hailo.prom"
  convert_and_check "$pass" "OCR detection" "OCR recognition" "layout"
  stop_server
done
cmp -s "$work/placement-1.prom" "$work/placement-2.prom" || fail "placement changed between boots"

if ls "$models" | grep -E '^(ch_|layout_|figure_)' ; then
  fail "an OCR, layout or figure ONNX file was in the device models directory"
fi
cp "$all_models/layout_heron.onnx" "$models/"
LAYOUT_MODEL=heron boot 3 "$golden_dir/hailo-query-layout.prom"
convert_and_check 3 "OCR detection" "OCR recognition"
stop_server
: > "$work/server.log"
echo "hailo-placement: ok"
