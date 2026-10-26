#!/usr/bin/env bash
# Cuts a fleet release from krick: one v<version> tag on gRParse and on every
# repository in release/fleet.tsv, Forgejo first, then GitHub
# (docs/RELEASING.md). The tags are what publish the versioned images; the
# GitHub Release follows on its own (.github/workflows/release.yml).
#
#   scripts/release.sh bump VERSION [--push]
#       Sets the project() version in CMakeLists.txt and commits
#       "Release VERSION". With --push, pushes main to both remotes. Wait
#       for that commit's CI to go green before tagging.
#   scripts/release.sh preflight VERSION
#       Read-only. For every fleet checkout: it exists, is clean, is on its
#       release branch, and that branch is identical locally, on Forgejo
#       and on GitHub; no remote has the tag yet; every GitHub check run on
#       the head commit finished green (gh api). A head with no check runs
#       at all fails too, since right after a push GitHub has not
#       registered them yet; ALLOW_NO_CI=1 accepts it for a repository that
#       runs CI only on pull requests. For gRParse, the CMake version
#       equals VERSION. Prints the commit each tag would point at.
#   scripts/release.sh tag VERSION [--yes]
#       Runs preflight, then (only with --yes) creates the annotated tags
#       and pushes each one to Forgejo, then to GitHub. Without --yes it
#       stops after printing the plan.
#
# The sibling checkouts sit next to this one, named after their
# repositories (FLEET_ROOT, default: the parent of this checkout, the same
# layout compose.stack.yaml's build: lines expect). Remote names default to
# origin (Forgejo) and github; FORGEJO_REMOTE and GITHUB_REMOTE override,
# and GITHUB_OWNER names the GitHub organization (default ai-pipestream).
# VERSION is MAJOR.MINOR.PATCH, digits only: CMake's project(VERSION) takes
# no pre-release suffix.
# Nothing here deletes or moves a tag; a mistake is fixed by hand.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fleet_file=${FLEET_FILE:-$root/release/fleet.tsv}
fleet_root=${FLEET_ROOT:-$(dirname "$root")}
forgejo=${FORGEJO_REMOTE:-origin}
github=${GITHUB_REMOTE:-github}
owner=${GITHUB_OWNER:-ai-pipestream}
semver='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'

usage() {
  sed -n '7,23p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 64
}

die() { echo "release: $*" >&2; exit 1; }

# repo branch, one per line, from fleet.tsv.
fleet() {
  sed 's/#.*//' "$fleet_file" | awk 'NF >= 3 { print $1, $2 }'
}

checkout_of() {
  if [[ $1 == gRParse ]]; then echo "$root"; else echo "$fleet_root/$1"; fi
}

cmd_bump() {
  local version=$1 push=${2:-}
  [[ -z $(git -C "$root" status --porcelain) ]] || die "gRParse checkout is not clean"
  [[ $(git -C "$root" rev-parse --abbrev-ref HEAD) == main ]] || die "gRParse is not on main"
  # Through a temporary file, since GNU and BSD sed take -i differently.
  local cmake=$root/CMakeLists.txt edited
  edited=$(mktemp)
  sed "s/^project(gRParse VERSION [0-9.]*/project(gRParse VERSION $version/" "$cmake" >"$edited"
  cat "$edited" >"$cmake"
  rm -f "$edited"
  "$root/scripts/cmake-version.sh" --expect "$version" >/dev/null \
    || die "CMakeLists.txt did not take version $version"
  git -C "$root" commit -q -am "Release $version"
  echo "committed $(git -C "$root" rev-parse --short HEAD) Release $version"
  if [[ $push == --push ]]; then
    git -C "$root" push -q "$forgejo" main
    git -C "$root" push -q "$github" main
  else
    echo "not pushed; push main to $forgejo and $github, then wait for green CI"
  fi
}

# Every check run on a GitHub commit must have finished, and none may have
# failed. A commit with no check runs at all yields "none", which preflight
# fails unless ALLOW_NO_CI=1.
ci_state() {
  local repo=$1 sha=$2
  # shellcheck disable=SC2016 # a jq program, not shell
  gh api "repos/$owner/$repo/commits/$sha/check-runs?per_page=100" --jq '
    [.check_runs[] | {name, status, conclusion}] as $runs
    | if ($runs | length) == 0 then "none"
      elif any($runs[]; .status != "completed") then
        "pending: " + ([$runs[] | select(.status != "completed") | .name] | join(", "))
      elif any($runs[]; (.conclusion // "") as $c | ($c != "success" and $c != "skipped" and $c != "neutral")) then
        "failed: " + ([$runs[] | select((.conclusion // "") as $c | ($c != "success" and $c != "skipped" and $c != "neutral")) | .name + "=" + (.conclusion // "none")] | join(", "))
      else "green" end'
}

cmd_preflight() {
  local version=$1 tag=v$1 problems=0 repo branch dir head remote remote_sha state ci
  command -v gh >/dev/null || die "gh is needed to read CI state on GitHub"
  "$root/scripts/cmake-version.sh" --expect "$version" >/dev/null \
    || { echo "gRParse: CMakeLists.txt is $("$root/scripts/cmake-version.sh"), not $version (run bump first)"; problems=$((problems + 1)); }
  printf '%-20s %-12s %-12s %s\n' repository branch commit state
  while read -r repo branch; do
    dir=$(checkout_of "$repo")
    if [[ ! -d $dir/.git ]]; then
      printf '%-20s %-12s %-12s %s\n' "$repo" "$branch" - "no checkout at $dir"
      problems=$((problems + 1)); continue
    fi
    state=()
    git -C "$dir" fetch -q "$forgejo" "$branch" --tags || state+=("fetch $forgejo failed")
    git -C "$dir" fetch -q "$github" "$branch" || state+=("fetch $github failed")
    [[ $(git -C "$dir" rev-parse --abbrev-ref HEAD) == "$branch" ]] || state+=("not on $branch")
    [[ -z $(git -C "$dir" status --porcelain) ]] || state+=("dirty")
    head=$(git -C "$dir" rev-parse HEAD)
    for remote in "$forgejo" "$github"; do
      remote_sha=$(git -C "$dir" rev-parse -q --verify "refs/remotes/$remote/$branch" || true)
      [[ $remote_sha == "$head" ]] || state+=("$remote/$branch is ${remote_sha:0:12}")
      if git -C "$dir" ls-remote --exit-code --tags "$remote" "refs/tags/$tag" >/dev/null 2>&1; then
        state+=("$tag already on $remote")
      fi
    done
    if git -C "$dir" rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
      state+=("$tag already exists locally")
    fi
    ci=$(ci_state "$repo" "$head" 2>/dev/null || echo "unreadable")
    case $ci in
      green) ;;
      none) [[ ${ALLOW_NO_CI:-0} == 1 ]] || state+=("CI none (no check runs yet; ALLOW_NO_CI=1 accepts it)") ;;
      *) state+=("CI $ci") ;;
    esac
    if [[ ${#state[@]} -eq 0 ]]; then
      printf '%-20s %-12s %-12s %s\n' "$repo" "$branch" "${head:0:12}" "ok (CI $ci)"
    else
      printf '%-20s %-12s %-12s %s\n' "$repo" "$branch" "${head:0:12}" "$(IFS=';'; echo "${state[*]}")"
      problems=$((problems + 1))
    fi
  done < <(fleet)
  if [[ $problems -gt 0 ]]; then
    die "$problems problem(s); nothing was tagged"
  fi
  echo "preflight: every repository is ready for $tag"
}

cmd_tag() {
  local version=$1 confirm=${2:-} tag=v$1 repo branch dir
  cmd_preflight "$version"
  if [[ $confirm != --yes ]]; then
    echo "dry run: rerun with --yes to create $tag in every repository above and push it"
    return 0
  fi
  while read -r repo branch; do
    dir=$(checkout_of "$repo")
    git -C "$dir" tag -a "$tag" -m "gRParse fleet $version"
    git -C "$dir" push -q "$forgejo" "refs/tags/$tag"
    git -C "$dir" push -q "$github" "refs/tags/$tag"
    echo "$repo: $tag pushed to $forgejo and $github"
  done < <(fleet)
  echo "tagged; the publish runs and then the release workflow take it from here"
}

[[ $# -ge 2 ]] || usage
command=$1 version=$2
shift 2
[[ $version =~ $semver ]] || die "'$version' is not MAJOR.MINOR.PATCH such as 0.3.0 (no pre-release suffix)"
[[ -f $fleet_file ]] || die "no fleet file at $fleet_file"
case $command in
  bump) [[ $# -le 1 ]] || usage; cmd_bump "$version" "${1:-}" ;;
  preflight) [[ $# -eq 0 ]] || usage; cmd_preflight "$version" ;;
  tag) [[ $# -le 1 ]] || usage; cmd_tag "$version" "${1:-}" ;;
  *) usage ;;
esac
