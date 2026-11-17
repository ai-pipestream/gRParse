#!/usr/bin/env bash
# OCR compile receipt for the Intel image: proves the OCR nets run a bounded
# set of input shapes on the GPU, so a document of a new size costs a few
# kernel compiles once and nothing after that.
#
#   scripts/test/gpu-ocr-shapes-test.sh <openvino-image> <models-dir>
#
# Needs a host with an Intel GPU render node (/dev/dri/renderD*); krick-1's
# Arc B70 is the fleet's. Without one the test FAILS; it never skips.
#
# The documents are the committed rasters under tests/data, each a different
# size, read in order. GRPARSE_OCR_SHAPES_EXTRA_DOCS (a colon list of raster
# paths, for example a 200 DPI letter page, the detector's largest input)
# adds more. Every document is converted twice on a cold cache, then once
# more after a restart on the same cache volume.
#
# Checks:
#   1. each document after the first adds at most GRPARSE_OCR_SHAPES_MAX_NEW_DET
#      (default 1) detector shapes and GRPARSE_OCR_SHAPES_MAX_NEW_REC (default
#      6) recognizer shapes to grparse_ocr_input_shapes, and at most
#      GRPARSE_OCR_SHAPES_MAX_NEW_BLOBS (default 64) files to the cache volume;
#   2. the second conversion of a document adds no shape and no cache file,
#      and takes at most GRPARSE_OCR_SHAPES_MAX_WARM_SECONDS (default 10);
#   3. the totals stay within GRPARSE_OCR_SHAPES_MAX_DET (default 9) and
#      GRPARSE_OCR_SHAPES_MAX_REC (default 14), the sizes of the bucket lists
#      in src/ocr_shape_buckets.cpp, and the angle net ran one shape;
#   4. after a restart on the warm cache every document converts without a
#      new cache file;
#   5. the server stays up, grparse_ort_ep_fallbacks_total reads 0, and its
#      log has no CL_OUT_OF_RESOURCES and no "Engine memory CAT error".
set -euo pipefail
image=${1:?openvino image}
models=${2:?models dir}
root=$(cd "$(dirname "$0")/../.." && pwd)
max_new_det=${GRPARSE_OCR_SHAPES_MAX_NEW_DET:-1}
max_new_rec=${GRPARSE_OCR_SHAPES_MAX_NEW_REC:-6}
max_new_blobs=${GRPARSE_OCR_SHAPES_MAX_NEW_BLOBS:-64}
max_warm_seconds=${GRPARSE_OCR_SHAPES_MAX_WARM_SECONDS:-10}
max_det=${GRPARSE_OCR_SHAPES_MAX_DET:-9}
max_rec=${GRPARSE_OCR_SHAPES_MAX_REC:-14}
work=$(mktemp -d)
server=grparse-gpu-ocr-shapes-$$
cache=grparse-gpu-ocr-shapes-cache-$$
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
fail() { echo "gpu-ocr-shapes: FAIL: $*" >&2; exit 1; }

# GRPARSE_RENDER_NODE names the render node for the script's own self-test
# (gpu-ocr-shapes-selftest.sh, a fake docker); a real run finds it.
render=${GRPARSE_RENDER_NODE:-$(ls /dev/dri/renderD* 2>/dev/null | head -1 || true)}
[ -n "$render" ] || fail "no /dev/dri/renderD* on this host; this test needs the Intel GPU"
gid=${GRPARSE_RENDER_GID:-$(stat -c %g "$render")}

docs=("$root/tests/data/report_page.png" "$root/tests/data/span_table.png"
      "$root/tests/data/bar_chart.png" "$root/tests/data/qr_code.png")
if [ -n "${GRPARSE_OCR_SHAPES_EXTRA_DOCS:-}" ]; then
  IFS=: read -r -a extra <<< "$GRPARSE_OCR_SHAPES_EXTRA_DOCS"
  docs+=("${extra[@]}")
fi
for doc in "${docs[@]}"; do [ -f "$doc" ] || fail "document missing: $doc"; done
# One directory the client container reads every document from.
mkdir "$work/docs"
for index in "${!docs[@]}"; do cp "${docs[$index]}" "$work/docs/$index-$(basename "${docs[$index]}")"; done

docker volume create "$cache" >/dev/null

boot() {
  docker run -d --name "$server" -p 127.0.0.1::9464 -v "$models:/models:ro" \
    -v "$cache:/var/cache/openvino" --device /dev/dri:/dev/dri --group-add "$gid" \
    -e GRPARSE_ORT_EP=openvino -e GRPARSE_PAGE_WORKERS=1 -e GRPARSE_METRICS_PORT=9464 \
    -e GRPARSE_OPENVINO_CACHE_DIR=/var/cache/openvino "$image" >/dev/null
  local listening=false
  for _ in $(seq 1 300); do
    if docker logs "$server" 2>&1 | grep -q "gRParse listening on"; then listening=true; break; fi
    [ "$(docker inspect -f '{{.State.Running}}' "$server")" = true ] || break
    sleep 1
  done
  [ "$listening" = true ] || fail "server did not come up"
  port=$(docker port "$server" 9464/tcp | head -1 | sed 's/.*://')
}

# shapes <net>: the server's grparse_ocr_input_shapes{net} value.
shapes() {
  curl -sf "http://127.0.0.1:$port/metrics" |
    awk -v series="grparse_ocr_input_shapes{net=\"$1\"}" '$1 == series {print $2}'
}
blobs() { docker exec "$server" sh -c 'find /var/cache/openvino -type f | wc -l'; }

# convert <document name>: one conversion; sets `seconds` to its wall time.
convert() {
  local started ended
  started=$(date +%s.%N)
  docker run --rm --network "container:$server" -v "$work/docs:/data:ro" \
    --entrypoint grparse-stream-client "$image" "/data/$1" localhost:50051 \
    > "$work/convert.out" 2>&1 || { cat "$work/convert.out"; fail "$1: conversion failed"; }
  ended=$(date +%s.%N)
  seconds=$(awk -v a="$started" -v b="$ended" 'BEGIN {printf "%.2f", b - a}')
  [ "$(docker inspect -f '{{.State.Running}}' "$server")" = true ] || fail "$1: the server died"
}

at_most() {  # at_most <value> <limit> <what>
  awk -v v="$1" -v l="$2" 'BEGIN {exit !(v <= l)}' || fail "$3: $1, limit $2"
}

echo "== cold cache: each document twice"
boot
first=true
for name in $(ls "$work/docs"); do
  det0=$(shapes det); rec0=$(shapes rec); blobs0=$(blobs)
  convert "$name"; cold=$seconds
  det1=$(shapes det); rec1=$(shapes rec); blobs1=$(blobs)
  convert "$name"; warm=$seconds
  det2=$(shapes det); rec2=$(shapes rec); blobs2=$(blobs)
  echo "$name: cold ${cold}s warm ${warm}s; det shapes +$((det1 - det0)), rec shapes +$((rec1 - rec0))," \
       "cache files +$((blobs1 - blobs0)); repeat: shapes +$((det2 - det1 + rec2 - rec1)), cache files +$((blobs2 - blobs1))"
  if [ "$first" != true ]; then
    at_most $((det1 - det0)) "$max_new_det" "$name: new detector shapes"
    at_most $((rec1 - rec0)) "$max_new_rec" "$name: new recognizer shapes"
    at_most $((blobs1 - blobs0)) "$max_new_blobs" "$name: new cache files"
  fi
  [ "$det2" = "$det1" ] && [ "$rec2" = "$rec1" ] || fail "$name: the repeat ran a new shape"
  [ "$blobs2" = "$blobs1" ] || fail "$name: the repeat wrote $((blobs2 - blobs1)) cache files"
  at_most "$warm" "$max_warm_seconds" "$name: warm conversion seconds"
  first=false
done
at_most "$(shapes det)" "$max_det" "detector shapes in total"
at_most "$(shapes rec)" "$max_rec" "recognizer shapes in total"
at_most "$(shapes cls)" 1 "angle net shapes in total"
cold_blobs=$(blobs)

check_health() {
  curl -sf "http://127.0.0.1:$port/metrics" | grep -qx 'grparse_ort_ep_fallbacks_total 0' ||
    fail "$1: grparse_ort_ep_fallbacks_total is not 0"
  if docker logs "$server" 2>&1 | grep -E "CL_OUT_OF_RESOURCES|Engine memory CAT error"; then
    fail "$1: the GPU faulted"
  fi
}
check_health "cold cache"
docker rm -f "$server" >/dev/null

echo "== restart on the warm cache"
boot
for name in $(ls "$work/docs"); do
  convert "$name"
  echo "$name: ${seconds}s"
done
warm_blobs=$(blobs)
echo "cache files: $cold_blobs after the cold pass, $warm_blobs after the warm restart"
[ "$warm_blobs" = "$cold_blobs" ] || fail "the warm restart wrote $((warm_blobs - cold_blobs)) cache files"
check_health "warm cache"
docker rm -f "$server" >/dev/null
echo "gpu-ocr-shapes: ok"
