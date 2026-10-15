#!/usr/bin/env bash
# End to end receipt for the typed OCR preset registry: starts the
# grparse-server image (built from this tree's Dockerfile, GPU via
# --gpus all) with a GRPARSE_CUSTOM_OCR_PRESETS registry and
# GRPARSE_ALLOWED_OCR_PRESETS=rapidocr (docling-serve #723's case), then
# calls ConvertSource over gRPC with grpcurl:
#   - the custom preset converts tests/data/report_page.png through RapidOCR;
#   - a built-in kind the allow list removed, and one never served, are
#     INVALID_ARGUMENT with a typed OptionRejection NOT_ALLOWED;
#   - a custom preset of a kind this server cannot run is NOT_IMPLEMENTED;
# and checks that a registry with an unreadable language stops startup.
#
# usage: scripts/test/ocr-presets-e2e-test.sh <image> <models-dir>
set -euo pipefail
image=${1:?grparse image}
models=${2:?models dir}
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
server=grparse-ocr-e2e-$$
cleanup() {
  echo "== server log"; docker logs "$server" 2>&1 || true
  docker rm -f "$server" >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT
run_server() { docker run --gpus all -v "$models:/models:ro" "$@"; }
presets='presets { key: "chinese_doc" value { kind: OCR_ENGINE_KIND_RAPIDOCR lang: "ch" lang: "en" } }
presets { key: "easy" value { kind: OCR_ENGINE_KIND_EASYOCR lang: "en" } }'

echo "== startup with an unreadable custom preset language must fail"
set +e
timeout 120 docker run --rm --gpus all -v "$models:/models:ro" \
  -e GRPARSE_CUSTOM_OCR_PRESETS='presets { key: "k" value { kind: OCR_ENGINE_KIND_RAPIDOCR lang: "korean" } }' \
  "$image" > "$work/bad.log" 2>&1
bad=$?
set -e
cat "$work/bad.log" | grep -i "ocr" || true
if [ "$bad" = 0 ] || ! grep -q "lang 'korean' is not a language the installed OCR models read" "$work/bad.log"; then
  echo "startup did not refuse the registry (exit $bad)" >&2; exit 1
fi
echo "refused at startup, exit $bad"

run_server -d --name "$server" -p 127.0.0.1::50051 \
  -e GRPARSE_ALLOWED_OCR_PRESETS=rapidocr -e GRPARSE_CUSTOM_OCR_PRESETS="$presets" "$image" >/dev/null
port=$(docker port "$server" 50051/tcp | head -1 | sed 's/.*://')
echo "== $image on 127.0.0.1:$port"
for _ in $(seq 1 120); do
  if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then break; fi
  sleep 1
done
sleep 3
docker logs "$server" 2>&1 | grep "OCR preset policy"
png=$(base64 -w0 "$root/tests/data/report_page.png")
call() {
  local preset=$1
  printf '{"request":{"sources":[{"file":{"filename":"report_page.png","base64_string":"%s"}}],"options":{"ocr_preset":"%s"}}}' \
    "$png" "$preset" > "$work/req.json"
  set +e
  grpcurl -plaintext -max-time 300 -format-error -d @ "127.0.0.1:$port" \
    ai.pipestream.parse.v1.ParseService/ConvertSource < "$work/req.json" > "$work/out-$preset.json" 2>&1
  local code=$?
  set -e
  echo "-- ocr_preset $preset: grpcurl exit $code"
  return 0
}
expect_refusal() {
  local preset=$1 reason=$2 text=$3
  call "$preset"
  python3 - "$work/out-$preset.json" "$reason" "$text" <<'PY'
import json, sys
out = json.load(open(sys.argv[1]))
print(json.dumps(out, indent=1))
assert out["code"] == 3, out  # INVALID_ARGUMENT
detail = out["details"][0]
assert detail["@type"].endswith("ai.pipestream.parse.v1.OptionRejection"), detail
assert detail["optionPath"] == "ocr_preset", detail
assert detail["reason"] == sys.argv[2], detail
assert sys.argv[3] in out["message"], out["message"]
PY
}

call chinese_doc
python3 - "$work/out-chinese_doc.json" <<'PY'
import json, sys
out = json.load(open(sys.argv[1]))["response"]
texts = [t.get("text", "") for t in out["document"]["doc"].get("texts", [])]
print("status:", out.get("status"), "texts:", len(texts))
print("first texts:", texts[:5])
assert out.get("status") == "CONVERSION_STATUS_SUCCESS", out.get("status")
assert len(texts) > 0 and any(t.strip() for t in texts), "RapidOCR read no text"
PY
expect_refusal auto OPTION_REJECTION_REASON_NOT_ALLOWED "ocr_preset 'auto' is not allowed. Allowed values: ['chinese_doc', 'default', 'easy', 'rapidocr']."
expect_refusal easyocr OPTION_REJECTION_REASON_NOT_ALLOWED "ocr_preset 'easyocr' is not allowed"
expect_refusal easy OPTION_REJECTION_REASON_NOT_IMPLEMENTED "OCR kind 'easyocr' (preset easy) is not available"
echo "ocr-presets-e2e: ok"
