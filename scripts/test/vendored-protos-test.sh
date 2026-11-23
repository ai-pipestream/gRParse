#!/bin/sh
# Every vendored contract is byte-identical to its owner's copy at the
# commit collectors/PINS.tsv names (#246): the collector and backend
# contracts under collectors/ and backends/, and every sibling's copy of
# document.proto (the fleet's source of truth lives here; a sibling that
# vendors it is pinned at the commit where its copy matches). Moving a pin is
# the explicit act of re-vendoring; editing a vendored file without moving
# its pin, or a document.proto change without the fleet sweep, fails here
# with the diff.
#
#   sh scripts/test/vendored-protos-test.sh               fetch from GitHub
#   VENDORED_PROTOS_FROM=<dir> sh ...                     read <dir>/<repo>/<path>
#   sh scripts/test/vendored-protos-test.sh --self-test   prove a drift fails
#
# Fetches raw files over HTTPS from the public repositories; no token.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
pins="$root/collectors/PINS.tsv"
base_url=${VENDORED_PROTOS_URL:-https://raw.githubusercontent.com/ai-pipestream}

check() {
  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  status=0
  rows=0
  # shellcheck disable=SC2034
  while IFS="$(printf '\t')" read -r file repo path commit; do
    case "$file" in ''|'#'*) continue ;; esac
    rows=$((rows + 1))
    if [ ! -f "$root/$file" ]; then
      echo "vendored-protos: $file is pinned but missing here" >&2
      status=1
      continue
    fi
    upstream="$work/$rows.proto"
    if [ -n "${VENDORED_PROTOS_FROM:-}" ]; then
      cp "$VENDORED_PROTOS_FROM/$repo/$path" "$upstream" 2>/dev/null || {
        echo "vendored-protos: $repo:$path not found under $VENDORED_PROTOS_FROM" >&2
        status=1
        continue
      }
    elif ! curl -fsSL --retry 3 -o "$upstream" "$base_url/$repo/$commit/$path"; then
      echo "vendored-protos: cannot fetch $repo@$commit:$path" >&2
      status=1
      continue
    fi
    if ! cmp -s "$root/$file" "$upstream"; then
      echo "vendored-protos: $file differs from $repo@$commit:$path" >&2
      diff -u "$upstream" "$root/$file" | head -40 >&2 || true
      status=1
    fi
  done < "$pins"
  if [ "$rows" -eq 0 ]; then
    echo "vendored-protos: $pins has no rows" >&2
    return 1
  fi
  [ "$status" -eq 0 ] && echo "vendored-protos: $rows pinned copies match"
  return "$status"
}

if [ "${1:-}" = "--self-test" ]; then
  # A local mirror built from our own files passes; one changed byte fails.
  mirror=$(mktemp -d)
  trap 'rm -rf "$mirror"' EXIT
  while IFS="$(printf '\t')" read -r file repo path commit; do
    case "$file" in ''|'#'*) continue ;; esac
    mkdir -p "$mirror/$repo/$(dirname "$path")"
    cp "$root/$file" "$mirror/$repo/$path"
  done < "$pins"
  VENDORED_PROTOS_FROM=$mirror sh "$0" >/dev/null || {
    echo "self-test: an identical mirror failed" >&2
    exit 1
  }
  first=$(grep -v '^#' "$pins" | head -1 | cut -f2,3 | tr '\t' '/')
  echo "// drift" >> "$mirror/$first"
  if VENDORED_PROTOS_FROM=$mirror sh "$0" >/dev/null 2>&1; then
    echo "self-test: a drifted copy passed" >&2
    exit 1
  fi
  echo "vendored-protos self-test: ok"
  exit 0
fi

check
