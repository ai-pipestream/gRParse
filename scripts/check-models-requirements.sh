#!/bin/sh
# Fails when scripts/models-requirements.txt has a pin with no binary wheel
# for the Python version of the Dockerfile.models fetch stage.
#
# The fetch stage is the only place those pins are installed, and the image
# workflows build it only on a models-all-tokenizer cache miss, so a Python
# base with no wheel for a pin stays green until the cache misses. This check
# does not use that cache: it reads the fetch FROM line and asks PyPI for
# wheels.
#
#   scripts/check-models-requirements.sh
#
# Exit 1 names every pin that has no wheel for manylinux_2_28_x86_64 or
# manylinux_2_28_aarch64 at that Python X.Y.
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
dockerfile=$root/Dockerfile.models
reqs=$root/scripts/models-requirements.txt

[ -f "$dockerfile" ] || {
  echo "check-models-requirements: no $dockerfile" >&2
  exit 1
}
[ -f "$reqs" ] || {
  echo "check-models-requirements: no $reqs" >&2
  exit 1
}

from=$(grep -E '^FROM[[:space:]].*[[:space:]]AS[[:space:]]fetch[[:space:]]*$' "$dockerfile" || true)
py=$(printf '%s\n' "$from" | sed -n 's/.*python:\([0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' | head -n 1)
if [ -z "$py" ]; then
  echo "check-models-requirements: could not parse Python X.Y from the fetch FROM line in $dockerfile" >&2
  printf 'check-models-requirements: %s\n' "${from:-<no AS fetch line>}" >&2
  exit 1
fi

if command -v pip >/dev/null 2>&1; then
  pip_bin=pip
elif command -v pip3 >/dev/null 2>&1; then
  pip_bin=pip3
elif python3 -m pip --version >/dev/null 2>&1; then
  pip_bin=
else
  echo "check-models-requirements: pip is required (pip, pip3, or python3 -m pip)" >&2
  exit 1
fi

run_pip() {
  if [ -n "$pip_bin" ]; then
    "$pip_bin" "$@"
  else
    python3 -m pip "$@"
  fi
}

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

export PIP_DISABLE_PIP_VERSION_CHECK=1
export PIP_NO_CACHE_DIR=1

echo "check-models-requirements: fetch stage Python $py ($from)"

status=0
seen=0
while IFS= read -r raw || [ -n "$raw" ]; do
  pin=$(printf '%s\n' "$raw" | sed 's/[[:space:]]*#.*//; s/^[[:space:]]*//; s/[[:space:]]*$//')
  [ -n "$pin" ] || continue
  seen=$((seen + 1))
  for platform in manylinux_2_28_x86_64 manylinux_2_28_aarch64; do
    log=$tmpdir/pip.log
    if run_pip download --only-binary=:all: --no-deps \
      --python-version "$py" --platform "$platform" \
      -d "$tmpdir/wheels" "$pin" >"$log" 2>&1; then
      continue
    fi
    status=1
    if grep -Eq 'No matching distribution|Could not find a version' "$log"; then
      echo "check-models-requirements: no wheel for $pin on Python $py ($platform)" >&2
    else
      echo "check-models-requirements: pip download failed for $pin on Python $py ($platform)" >&2
    fi
    cat "$log" >&2
  done
done <"$reqs"

if [ "$seen" -eq 0 ]; then
  echo "check-models-requirements: no pins in $reqs" >&2
  exit 1
fi

if [ "$status" -ne 0 ]; then
  echo "check-models-requirements: Python $py cannot install scripts/models-requirements.txt" >&2
  exit 1
fi

echo "check-models-requirements: Python $py wheels present for manylinux_2_28_x86_64 and manylinux_2_28_aarch64"
