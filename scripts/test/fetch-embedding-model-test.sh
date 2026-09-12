#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
fetch=$repo_root/scripts/fetch-embedding-model.sh
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/upstream"
printf 'tokenizer bytes\n' >"$work/upstream/tokenizer.json"
printf 'onnx bytes\n' >"$work/upstream/model.onnx"
printf 'xml bytes\n' >"$work/upstream/openvino_model.xml"
printf 'bin bytes\n' >"$work/upstream/openvino_model.bin"

manifest=$work/MANIFEST
for item in 'tokenizer.json common' 'model.onnx onnx' \
  'openvino_model.xml openvino' 'openvino_model.bin openvino'; do
  read -r path group <<< "$item"
  printf '%s %s %s %s Apache-2.0 file://%s\n' \
    "$path" "$(sha256sum "$work/upstream/$path" | cut -d' ' -f1)" \
    "$(stat -c %s "$work/upstream/$path")" "$group" "$work/upstream/$path" \
    >>"$manifest"
done

dir=$work/models
"$fetch" --manifest "$manifest" --dir "$dir" --backend onnx
test -f "$dir/tokenizer.json"
test -f "$dir/model.onnx"
test ! -e "$dir/openvino_model.xml"
"$fetch" --verify --manifest "$manifest" --dir "$dir" --backend onnx

printf 'corrupt\n' >>"$dir/model.onnx"
if "$fetch" --verify --manifest "$manifest" --dir "$dir" --backend onnx; then
  echo "verify accepted a corrupt artifact" >&2
  exit 1
fi
"$fetch" --manifest "$manifest" --dir "$dir" --backend all
"$fetch" --verify --manifest "$manifest" --dir "$dir" --backend all

# Verify must not execute curl, even when required files are missing.
# Exported below for indirect invocation by the fetcher's child Bash process.
# shellcheck disable=SC2329
curl() { echo 'unexpected curl invocation' >&2; return 99; }
export -f curl
"$fetch" --verify --manifest "$manifest" --dir "$dir" --backend all
if "$fetch" --verify --manifest "$manifest" --dir "$work/absent"; then
  echo 'verify accepted missing artifacts' >&2
  exit 1
fi
test ! -e "$work/absent"
unset -f curl

# Equal-size wrong bytes exercise SHA256 rather than just the size check.
printf 'bad! bytes\n' >"$work/upstream/model.onnx"
printf 'existing target\n' >"$dir/model.onnx"
cp "$dir/model.onnx" "$work/previous"
if "$fetch" --manifest "$manifest" --dir "$dir" --backend onnx; then
  echo 'fetch accepted incorrect upstream bytes' >&2
  exit 1
fi
cmp "$work/previous" "$dir/model.onnx"
shopt -s nullglob
parts=("$dir"/*.part.*)
test "${#parts[@]}" -eq 0

"$fetch" --manifest "$manifest" --dir "$work/intel" --backend openvino
test -f "$work/intel/tokenizer.json"
test -f "$work/intel/openvino_model.xml"
test -f "$work/intel/openvino_model.bin"
test ! -e "$work/intel/model.onnx"

echo "fetch-embedding-model-test: OK"
