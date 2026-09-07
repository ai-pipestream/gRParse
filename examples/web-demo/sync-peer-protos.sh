#!/usr/bin/env bash
# Vendors every peer's proto include tree into peer-protos/<repo>/<include>/
# so the demo shell (and its image) loads the fleet's contracts without
# sibling checkouts. The list of trees comes from peers.js, the same
# registry server.js dials with, so the two cannot disagree.
#
#   ./sync-peer-protos.sh                  copy from the sibling checkouts
#   ./sync-peer-protos.sh --check          exit 1 and name every drifted path
#   ./sync-peer-protos.sh --workspace DIR  siblings live under DIR/<repo>
#
# The workspace is found automatically for the two layouts the shell itself
# probes: a plain checkout (<ws>/gRParse/examples/web-demo) and a git
# worktree one level deeper (<ws>/worktrees/gRParse-x/examples/web-demo).
# Each vendored tree is a byte-identical copy; never hand-edit one. A peer
# contract change is followed by this script, a commit, and a shell rebuild.
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
vendored="$here/peer-protos"
mode=sync
workspace=""

while [ $# -gt 0 ]; do
  case "$1" in
    --check) mode=check ;;
    --workspace) shift; workspace="${1:-}" ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

trees="$(node "$here/peers.js" --list)"
first_repo="$(printf '%s\n' "$trees" | head -n1 | cut -f1)"

find_workspace() {
  local candidate
  for candidate in "$here/../../.." "$here/../../../.."; do
    if [ -d "$candidate/$first_repo" ]; then
      (cd -- "$candidate" && pwd)
      return 0
    fi
  done
  return 1
}

if [ -z "$workspace" ]; then
  workspace="$(find_workspace)" || {
    echo "no sibling checkouts found above $here; pass --workspace <dir>" >&2
    exit 2
  }
fi

# Collects the repos whose document.proto copy differs from gRParse's own
# (the fleet source of truth). Informational: the sibling owns the fix.
document_drift=""
note_document_drift() {
  local repo="$1" include="$2"
  local copy="$workspace/$repo/$include/ai/pipestream/document/v1/document.proto"
  [ -f "$copy" ] || return 0
  cmp -s "$copy" "$here/../../document.proto" || document_drift="$document_drift $repo"
}

drift=0
while IFS=$'\t' read -r repo include; do
  source="$workspace/$repo/$include"
  target="$vendored/$repo/$include"
  if [ ! -d "$source" ]; then
    echo "missing: $source" >&2
    drift=1
    continue
  fi
  note_document_drift "$repo" "$include"
  if [ "$mode" = check ]; then
    if [ ! -d "$target" ]; then
      echo "drift: $target is absent"
      drift=1
    elif ! diff -rq "$source" "$target" >/dev/null; then
      diff -rq "$source" "$target" | sed 's/^/drift: /'
      drift=1
    fi
  else
    rm -rf "$target"
    mkdir -p "$(dirname -- "$target")"
    cp -R -- "$source" "$target"
    echo "synced $repo/$include"
  fi
done <<< "$trees"

# Directories under peer-protos/ that no registry entry names are stale.
for dir in "$vendored"/*/; do
  [ -d "$dir" ] || continue
  name="$(basename -- "$dir")"
  if ! printf '%s\n' "$trees" | cut -f1 | grep -qx -- "$name"; then
    if [ "$mode" = check ]; then
      echo "drift: $dir is not in the registry"
      drift=1
    else
      rm -rf -- "$dir"
      echo "removed stale $name"
    fi
  fi
done

if [ -n "$document_drift" ]; then
  echo "note: document.proto differs from gRParse's in:$document_drift (fleet sweep pending; the sibling owns the fix)"
fi

if [ "$mode" = check ]; then
  if [ "$drift" -ne 0 ]; then
    echo "peer-protos/ drifts from $workspace; run $0" >&2
    exit 1
  fi
  echo "peer-protos/ matches $workspace"
else
  [ "$drift" -eq 0 ] || exit 1
  node "$here/peers.js" --check-vendored
fi
