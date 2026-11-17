#!/usr/bin/env bash
# Runs scripts/test/gpu-ocr-shapes-test.sh against a fake docker and curl, so
# the receipt's own arithmetic is tested where there is no GPU: a server whose
# shapes and cache files stay bounded passes, and one that adds too many
# detector shapes, writes cache files on a repeat or after a restart, or logs
# a GPU fault fails with the matching message.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/bin" "$work/models"

# The fake server: every document runs the shapes its scenario line lists
# ("<name-suffix> <shape> <shape> ..."). Shapes are counted per boot, the way
# the server's ledger counts them, and a shape adds FAKE_FILES_PER_SHAPE cache
# files the first time any boot runs it, the way the plugin's cache does.
cat > "$work/bin/docker" <<'FAKE'
#!/usr/bin/env bash
state=$FAKE_STATE
case "$1" in
  volume|port|logs|inspect|exec|rm|run) ;;
  *) echo "fake docker: unexpected $*" >&2; exit 2 ;;
esac
case "$1" in
  volume) exit 0 ;;
  port) echo "127.0.0.1:19464" ;;
  logs) echo "gRParse listening on 0.0.0.0:50051"; cat "$state/log" 2>/dev/null || true ;;
  inspect)
    [ -f "$state/running" ] || exit 1
    [ "$2" = -f ] && echo true
    exit 0 ;;
  exec) wc -l < "$state/cache" ;;
  rm) rm -f "$state/running" ;;
  run)
    if [ "$2" = -d ]; then
      : > "$state/boot"; touch "$state/running" "$state/cache"
      [ -z "${FAKE_BOOT_FILES:-}" ] || [ ! -f "$state/booted" ] || echo "boot" >> "$state/cache"
      touch "$state/booted"
      exit 0
    fi
    document=""
    for argument in "$@"; do case "$argument" in /data/*) document=${argument#/data/} ;; esac; done
    shapes=$(awk -v d="$document" 'index(d, $1) { $1 = ""; print }' "$FAKE_SCENARIO")
    for shape in $shapes; do
      grep -qx "$shape" "$state/boot" || echo "$shape" >> "$state/boot"
      if ! grep -qx "$shape" "$state/cache"; then
        for _ in $(seq 1 "${FAKE_FILES_PER_SHAPE:-3}"); do echo "$shape" >> "$state/cache"; done
      fi
    done
    [ -z "${FAKE_LEAK:-}" ] || echo "leak" >> "$state/cache"
    [ -z "${FAKE_FAULT:-}" ] || echo "[GPU] clFinish, error code: -5 CL_OUT_OF_RESOURCES" >> "$state/log"
    ;;
esac
FAKE
cat > "$work/bin/curl" <<'FAKE'
#!/usr/bin/env bash
for net in det cls rec; do
  echo "grparse_ocr_input_shapes{net=\"$net\"} $(grep -c "^$net:" "$FAKE_STATE/boot" || true)"
done
echo "grparse_ort_ep_fallbacks_total 0"
FAKE
chmod +x "$work/bin/docker" "$work/bin/curl"

# run <label> <scenario text> [NAME=value ...]: one receipt run; prints its
# output and returns its exit code.
run() {
  local label=$1 scenario=$2; shift 2
  rm -rf "$work/state"; mkdir "$work/state"
  printf '%s\n' "$scenario" > "$work/scenario"
  env PATH="$work/bin:$PATH" FAKE_STATE="$work/state" FAKE_SCENARIO="$work/scenario" \
    GRPARSE_RENDER_NODE=/dev/null GRPARSE_RENDER_GID=0 "$@" \
    bash "$root/scripts/test/gpu-ocr-shapes-test.sh" fake-image "$work/models" > "$work/$label.out" 2>&1
}
fail=0
expect_failure() {  # label, text the failure must carry
  if [ "$3" = 0 ]; then echo "FAIL: $1 passed" >&2; fail=1; return; fi
  grep -qF "$2" "$work/$1.out" || { echo "FAIL: $1 does not say '$2'" >&2; cat "$work/$1.out" >&2; fail=1; }
}

bounded='report_page det:1664x2144 cls:192 rec:64 rec:96 rec:128 rec:1024 rec:1280
span_table det:1088x544 cls:192 rec:96 rec:192 rec:256
bar_chart det:640x480 cls:192 rec:64 rec:96
qr_code det:320x320 cls:192'
run bounded "$bounded" || { echo "FAIL: a bounded server failed" >&2; cat "$work/bounded.out" >&2; fail=1; }
grep -q "gpu-ocr-shapes: ok" "$work/bounded.out" || { echo "FAIL: no ok line" >&2; fail=1; }
grep -q "span_table.png: .*det shapes +1, rec shapes +2, cache files +9" "$work/bounded.out" ||
  { echo "FAIL: the second document's deltas are not reported" >&2; cat "$work/bounded.out" >&2; fail=1; }

code=0; run free-sizes "${bounded/det:1088x544/det:1088x544 det:736x352 det:704x384}" || code=$?
expect_failure free-sizes "span_table.png: new detector shapes: 3, limit 1" "$code"
code=0; run leak "$bounded" FAKE_LEAK=1 || code=$?
expect_failure leak "the repeat wrote 1 cache files" "$code"
code=0; run restart "$bounded" FAKE_BOOT_FILES=1 || code=$?
expect_failure restart "the warm restart wrote 1 cache files" "$code"
code=0; run fault "$bounded" FAKE_FAULT=1 || code=$?
expect_failure fault "the GPU faulted" "$code"
code=0; run blobs "$bounded" FAKE_FILES_PER_SHAPE=40 || code=$?
expect_failure blobs "span_table.png: new cache files: 120, limit 64" "$code"

[ "$fail" -eq 0 ] && echo "gpu-ocr-shapes-selftest: ok"
exit "$fail"
