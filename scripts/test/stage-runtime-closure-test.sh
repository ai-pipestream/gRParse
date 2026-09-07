#!/bin/sh
# Checks scripts/stage-runtime-closure.sh against the host's own binaries,
# so it runs anywhere with glibc and ldd (the Docker build stages included,
# through ctest).
#
#   sh scripts/test/stage-runtime-closure-test.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
stage="$here/../stage-runtime-closure.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0

fail() {
  echo "FAIL: $1" >&2
  failures=$((failures + 1))
}

# A dynamically linked program that every glibc system has.
subject=$(command -v sh)
subject=$(readlink -f "$subject")

echo "== the closure of $subject lands in the output directory, glibc stays out"
if sh "$stage" "$work/out" "$subject" > "$work/log"; then
  ldd "$subject" | awk '/=> \// {print $3}' | while read -r lib; do
    name=$(basename "$lib")
    case "$name" in
      libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*)
        [ ! -e "$work/out/$name" ] || echo "glibc member staged: $name" ;;
      *)
        [ -e "$work/out/$name" ] || echo "missing from the stage: $name" ;;
    esac
  done > "$work/mismatch"
  [ ! -s "$work/mismatch" ] || { cat "$work/mismatch" >&2; fail "staged set differs from ldd"; }
  grep -q '^stage-runtime-closure: staged [0-9]* libraries' "$work/log" || fail "no summary line"
  for staged in "$work"/out/*; do
    [ -e "$staged" ] || continue
    [ ! -L "$staged" ] || fail "symlink copied instead of the file: $staged"
  done
else
  fail "staging the closure of $subject exited nonzero"
fi

echo "== STAGE_KEEP_ON_BASE excludes a family the base owns"
if STAGE_KEEP_ON_BASE='.*' sh "$stage" "$work/kept" "$subject" > /dev/null; then
  [ -z "$(ls -A "$work/kept")" ] || fail "libraries staged despite a catch-all keep regex"
else
  fail "keep regex run exited nonzero"
fi

echo "== a missing input fails"
if sh "$stage" "$work/missing" "$work/does-not-exist" > /dev/null 2>&1; then
  fail "a nonexistent input was accepted"
fi

echo "== usage errors exit 64"
sh "$stage" > /dev/null 2>&1 && fail "no arguments accepted" || [ $? -eq 64 ] || fail "wrong usage exit code"

if [ "$failures" -ne 0 ]; then
  echo "stage-runtime-closure-test: $failures failure(s)" >&2
  exit 1
fi
echo "stage-runtime-closure-test: OK"
