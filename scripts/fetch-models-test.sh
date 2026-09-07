#!/usr/bin/env bash
# Offline test of scripts/fetch-models.sh and scripts/models-populate.sh:
# a private manifest with file:// URLs and a stand-in patch command, so no
# network, no onnx, no real model bytes. Exit 0 when every case holds.
#
#   scripts/fetch-models-test.sh
#
# Cases: fresh fetch (download + patch), a second run copies nothing,
# --verify passes on the fetched dir, fails on a corrupted file and on a
# missing required file, a wrong upstream is refused and removed, --layout
# and --tokenizer select the right rows, and the populate script fills an
# empty target from a source and passes verification.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
fetch=$script_dir/fetch-models.sh
populate=$script_dir/models-populate.sh
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

failures=0
check() {  # check <name> <expected exit> <command...>
  local name=$1 expected=$2 actual=0
  shift 2
  # The command writes to out.log; last.log keeps the previous command's
  # output so a follow-up check can grep what the command before it said.
  "$@" >"$work/out.log" 2>&1 || actual=$?
  if [ "$actual" -ne "$expected" ]; then
    failures=$((failures + 1))
    echo "FAIL $name: exit $actual, expected $expected" >&2
    cat "$work/out.log" >&2
  else
    echo "ok   $name"
  fi
  mv "$work/out.log" "$work/last.log"
}

sha_of() { sha256sum "$1" | cut -c1-64; }

# Upstream stand-ins served over file://; the "patched" file is the source
# with a trailer, which the fake python below produces from any source.
mkdir -p "$work/upstream/sub"
printf 'det bytes\n' >"$work/upstream/det.onnx"
printf 'keys\n' >"$work/upstream/keys.txt"
printf 'heron\n' >"$work/upstream/heron.onnx"
printf 'picodet\n' >"$work/upstream/picodet.onnx"
printf 'figure upstream\n' >"$work/upstream/figure_up.onnx"
printf 'figure upstream\npatched\n' >"$work/patched.onnx"
printf '{"tok":1}\n' >"$work/upstream/sub/tokenizer.json"

cat >"$work/fakepython" <<'EOF'
#!/usr/bin/env bash
# stands in for `python patch_figure_classifier.py SOURCE DEST`
{ cat "$2"; printf 'patched\n'; } >"$3"
EOF
chmod +x "$work/fakepython"
export FETCH_MODELS_PYTHON=$work/fakepython

manifest=$work/MANIFEST
{
  echo "# test manifest"
  printf '%s %s %s %s %s %s\n' det.onnx "$(sha_of "$work/upstream/det.onnx")" 10 ocr X "file://$work/upstream/det.onnx"
  printf '%s %s %s %s %s %s\n' keys.txt "$(sha_of "$work/upstream/keys.txt")" 5 ocr X "file://$work/upstream/keys.txt"
  printf '%s %s %s %s %s %s\n' heron.onnx "$(sha_of "$work/upstream/heron.onnx")" 6 layout-heron X "file://$work/upstream/heron.onnx"
  printf '%s %s %s %s %s %s\n' picodet.onnx "$(sha_of "$work/upstream/picodet.onnx")" 8 layout-picodet X "file://$work/upstream/picodet.onnx"
  printf '%s %s %s %s %s %s\n' figure_up.onnx "$(sha_of "$work/upstream/figure_up.onnx")" 16 figure-upstream X "file://$work/upstream/figure_up.onnx"
  printf '%s %s %s %s %s %s\n' figure.onnx "$(sha_of "$work/patched.onnx")" 24 figure X patch:figure_up.onnx
  printf '%s %s %s %s %s %s\n' chunk/tokenizer.json "$(sha_of "$work/upstream/sub/tokenizer.json")" 10 tokenizer X "file://$work/upstream/sub/tokenizer.json"
} >"$manifest"

dir=$work/models
check "fresh fetch" 0 "$fetch" --manifest "$manifest" --dir "$dir"
check "fetched files present" 0 test -f "$dir/det.onnx" -a -f "$dir/heron.onnx" -a -f "$dir/figure.onnx"
check "patched bytes" 0 cmp "$dir/figure.onnx" "$work/patched.onnx"
check "picodet not fetched by default" 1 test -f "$dir/picodet.onnx"
check "tokenizer not fetched by default" 1 test -f "$dir/chunk/tokenizer.json"
check "second run is a no-op" 0 "$fetch" --manifest "$manifest" --dir "$dir"
check "no-op run reports every file present" 0 bash -c "! grep -qE '(downloaded|patched)' '$work/last.log'"
check "verify passes" 0 "$fetch" --verify --manifest "$manifest" --dir "$dir"
check "layout all adds picodet" 0 "$fetch" --manifest "$manifest" --dir "$dir" --layout all
check "picodet now present" 0 test -f "$dir/picodet.onnx"
check "tokenizer opt-in" 0 "$fetch" --manifest "$manifest" --dir "$dir" --tokenizer
check "tokenizer now present" 0 test -f "$dir/chunk/tokenizer.json"

printf 'corrupt\n' >>"$dir/heron.onnx"
check "verify fails on a corrupted file" 1 "$fetch" --verify --manifest "$manifest" --dir "$dir"
check "corruption named" 0 grep -q 'FAIL heron.onnx' "$work/last.log"
check "fetch replaces the corrupted file" 0 "$fetch" --manifest "$manifest" --dir "$dir"
check "verify passes again" 0 "$fetch" --verify --manifest "$manifest" --dir "$dir"

rm "$dir/det.onnx"
check "verify fails on a missing required file" 1 "$fetch" --verify --manifest "$manifest" --dir "$dir"
check "missing file named" 0 grep -q 'FAIL det.onnx: required file is missing' "$work/last.log"
rm "$dir/heron.onnx"
check "verify tolerates a missing optional file" 1 "$fetch" --verify --manifest "$manifest" --dir "$dir"
check "optional file reported absent, not failed" 0 bash -c "grep -q 'heron.onnx *absent' '$work/last.log' && ! grep -q 'FAIL heron' '$work/last.log'"

printf 'tampered\n' >"$work/upstream/det.onnx"
check "wrong upstream bytes are refused" 1 "$fetch" --manifest "$manifest" --dir "$dir"
check "refused download is not left behind" 1 test -f "$dir/det.onnx"
check "usage error on a bad layout" 64 "$fetch" --manifest "$manifest" --dir "$dir" --layout other

# populate: a source dir laid out like the image's /models, an empty target.
printf 'det bytes\n' >"$work/upstream/det.onnx"
src=$work/image-models
check "fetch a full source for populate" 0 "$fetch" --manifest "$manifest" --dir "$src" --layout all --tokenizer
cp "$manifest" "$src/MANIFEST"
target=$work/volume
check "populate fills an empty target" 0 env MODELS_SOURCE="$src" "$populate" "$target"
check "populate copied the tokenizer" 0 cmp "$src/chunk/tokenizer.json" "$target/chunk/tokenizer.json"
check "populate skipped the manifest" 1 test -f "$target/MANIFEST"
check "populate again copies nothing" 0 env MODELS_SOURCE="$src" "$populate" "$target"
check "populate reports zero copies" 0 grep -q 'populate: 0 file(s) copied' "$work/last.log"
printf 'x' >>"$target/keys.txt"
check "populate repairs a changed file" 0 env MODELS_SOURCE="$src" "$populate" "$target"
check "populate reports one copy" 0 grep -q 'populate: 1 file(s) copied' "$work/last.log"

if [ "$failures" -gt 0 ]; then
  echo "$failures check(s) failed" >&2
  exit 1
fi
echo "fetch-models-test: OK"
