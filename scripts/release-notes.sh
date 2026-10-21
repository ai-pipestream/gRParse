#!/usr/bin/env bash
# Prints the gRParse half of a release's notes as markdown: every commit
# between two tags, grouped by what it touched. Much of gRParse lands as
# direct commits rather than pull requests, so GitHub's generated notes
# (built from PR titles) would miss most of a release; the commit log does
# not. The release workflow adds the fleet and image tables after this.
#
#   scripts/release-notes.sh v0.2.1 v0.3.0
#
# A commit that changes a .proto file goes under "Contract", since that is
# what decides minor versus patch (docs/RELEASING.md). One that touches only
# docs or Markdown goes under "Documentation", one that touches only CI,
# workflows or scripts under "Build, CI and release", the rest under
# "Changes". Merge commits are left out.
set -euo pipefail

[[ $# -eq 2 ]] || { echo "Usage: $0 PREVIOUS_TAG TAG" >&2; exit 64; }
previous=$1 tag=$2
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
git -C "$root" rev-parse -q --verify "$previous^{commit}" >/dev/null || { echo "no $previous in this clone" >&2; exit 1; }
git -C "$root" rev-parse -q --verify "$tag^{commit}" >/dev/null || { echo "no $tag in this clone" >&2; exit 1; }

declare -A section=()
order=("Contract" "Changes" "Build, CI and release" "Documentation")
count=0
while read -r sha; do
  subject=$(git -C "$root" log -1 --format=%s "$sha")
  files=$(git -C "$root" diff-tree --no-commit-id --name-only -r "$sha")
  if grep -q '\.proto$' <<<"$files"; then
    group="Contract"
  elif [[ -n $files ]] && ! grep -qvE '^docs/|\.md$' <<<"$files"; then
    group="Documentation"
  elif [[ -n $files ]] && ! grep -qvE '^\.github/|^scripts/|^\.forgejo/|^renovate\.json' <<<"$files"; then
    group="Build, CI and release"
  else
    group="Changes"
  fi
  section[$group]+="- ${subject} (${sha:0:9})"$'\n'
  count=$((count + 1))
done < <(git -C "$root" rev-list --no-merges --reverse "$previous..$tag")

echo "## gRParse since $previous"
echo
echo "$count commits. Full diff: https://github.com/ai-pipestream/gRParse/compare/$previous...$tag"
for group in "${order[@]}"; do
  [[ -n ${section[$group]:-} ]] || continue
  echo
  echo "### $group"
  echo
  printf '%s' "${section[$group]}"
done
