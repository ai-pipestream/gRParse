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
#       and on GitHub; no remote has the tag yet; CI on the head commit
#       finished green on GitHub (every check run, gh api) and on Forgejo
#       (the combined commit status). For gRParse, the CMake version equals
#       VERSION. Prints the commit each tag would point at.
#   scripts/release.sh tag VERSION [--yes]
#       Runs preflight, then (only with --yes) creates the annotated tags
#       and pushes each one to Forgejo, then to GitHub. Without --yes it
#       stops after printing the plan.
#
# VERSION is x.y.z or a prerelease x.y.z-<suffix> such as 0.3.0-rc.1. CMake
# holds only x.y.z, so bump writes that and a prerelease tags the tree whose
# CMake version is its x.y.z.
#
# The sibling checkouts sit next to this one, named after their
# repositories (FLEET_ROOT, default: the parent of this checkout, the same
# layout compose.stack.yaml's build: lines expect). Remote names default to
# origin (Forgejo) and github; FORGEJO_REMOTE and GITHUB_REMOTE override.
# The Forgejo API is FORGEJO_URL (default https://git.rokkon.com) with the
# token in FORGEJO_TOKEN, else FORGEJO_PAT from ~/.config/forgejo/env.
#
# Tags go on one repository after another. If a push fails partway, the
# script stops and prints which repositories carry the tag and which do
# not; rerunning tag skips nothing, so finish the rest by hand (git tag -a
# and push to Forgejo, then GitHub). Nothing here deletes or moves a tag.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fleet_file=${FLEET_FILE:-$root/release/fleet.tsv}
fleet_root=${FLEET_ROOT:-$(dirname "$root")}
forgejo=${FORGEJO_REMOTE:-origin}
github=${GITHUB_REMOTE:-github}
owner=${GITHUB_OWNER:-ai-pipestream}
forgejo_url=${FORGEJO_URL:-https://git.rokkon.com}
forgejo_owner=${FORGEJO_OWNER:-ai-pipestream}
semver='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z][0-9A-Za-z.-]{0,63})?$'

usage() {
  sed -n '7,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
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
  local version=${1%%-*} push=${2:-}
  [[ -z $(git -C "$root" status --porcelain) ]] || die "gRParse checkout is not clean"
  [[ $(git -C "$root" rev-parse --abbrev-ref HEAD) == main ]] || die "gRParse is not on main"
  [[ $("$root/scripts/cmake-version.sh") != "$version" ]] \
    || die "CMakeLists.txt is already $version; nothing to bump (tag it, or preflight it)"
  sed "s/^project(gRParse VERSION [0-9.]*/project(gRParse VERSION $version/" \
    "$root/CMakeLists.txt" >"$root/CMakeLists.txt.tmp"
  mv "$root/CMakeLists.txt.tmp" "$root/CMakeLists.txt"
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

# GitHub CI on a commit: every check run, all pages. "green" when every run
# completed with conclusion success, skipped or neutral (a job whose `if:`
# was false, or a check that reports without passing judgement); "pending:"
# or "failed:" with the names otherwise (failure, cancelled, timed_out,
# action_required, stale and startup_failure all fail); "none" with no
# check runs at all.
github_ci() {
  local repo=$1 sha=$2 runs name status conclusion pending=() failed=()
  # shellcheck disable=SC2016 # a jq program, not shell
  runs=$(gh api --paginate "repos/$owner/$repo/commits/$sha/check-runs?per_page=100" \
    --jq '.check_runs[] | [.name, .status, (.conclusion // "none")] | @tsv') || return 1
  [[ -n $runs ]] || { echo none; return 0; }
  while IFS=$'\t' read -r name status conclusion; do
    if [[ $status != completed ]]; then
      pending+=("$name")
    else
      case $conclusion in
        success | skipped | neutral) ;;
        *) failed+=("$name=$conclusion") ;;
      esac
    fi
  done <<<"$runs"
  if [[ ${#pending[@]} -gt 0 ]]; then
    echo "pending: $(IFS=,; echo "${pending[*]}")"
  elif [[ ${#failed[@]} -gt 0 ]]; then
    echo "failed: $(IFS=,; echo "${failed[*]}")"
  else
    echo green
  fi
}

# A curl --config file carrying the Forgejo token, mode 600, so the token
# is never on a command line.
forgejo_auth() {
  local token=${FORGEJO_TOKEN:-}
  if [[ -z $token && -f $HOME/.config/forgejo/env ]]; then
    # shellcheck disable=SC1091 # krick's credential file, not in the repo
    token=$(set -a; . "$HOME/.config/forgejo/env"; printf '%s' "${FORGEJO_PAT:-}")
  fi
  [[ -n $token ]] || return 1
  forgejo_config=$(mktemp)
  chmod 600 "$forgejo_config"
  printf 'header = "Authorization: token %s"\n' "$token" >"$forgejo_config"
}

# Forgejo CI on a commit: the latest status of every context, all pages.
# "green" when each is success or skipped (a job whose `if:` was false);
# "pending:" with the pending contexts; "failed:" with the rest (failure,
# error, warning); "none" with no statuses at all.
forgejo_ci() {
  local repo=$1 sha=$2 page=1 body seen=0 total
  local -a bodies=()
  while :; do
    body=$(curl -fsS --config "$forgejo_config" \
      "$forgejo_url/api/v1/repos/$forgejo_owner/$repo/commits/$sha/status?limit=50&page=$page") || return 1
    bodies+=("$body")
    read -r seen total < <(python3 -c '
import json, sys
page = json.loads(sys.argv[2])
print(int(sys.argv[1]) + len(page["statuses"]), page["total_count"])
' "$seen" "$body")
    [[ $seen -lt $total && $page -lt 100 ]] || break
    page=$((page + 1))
  done
  python3 -c '
import json, sys
statuses = [s for body in sys.argv[1:] for s in json.loads(body)["statuses"]]
pending = sorted({s["context"] for s in statuses if s["status"] == "pending"})
failed = sorted({s["context"] + "=" + s["status"] for s in statuses
                 if s["status"] not in ("success", "skipped", "pending")})
if not statuses:
    print("none")
elif pending:
    print("pending: " + ",".join(pending))
elif failed:
    print("failed: " + ",".join(failed))
else:
    print("green")
' "${bodies[@]}"
}

cmd_preflight() {
  local version=$1 tag=v$1 problems=0 repo branch dir head remote_sha state ci forge
  command -v gh >/dev/null || die "gh is needed to read CI state on GitHub"
  forgejo_auth || die "no Forgejo token (FORGEJO_TOKEN, or FORGEJO_PAT in ~/.config/forgejo/env) to read CI state on Forgejo"
  trap 'rm -f "$forgejo_config"' EXIT
  "$root/scripts/cmake-version.sh" --expect "$version" >/dev/null \
    || { echo "gRParse: CMakeLists.txt is $("$root/scripts/cmake-version.sh"), not ${version%%-*} (run bump first)"; problems=$((problems + 1)); }
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
    for forge in GitHub Forgejo; do
      if [[ $forge == GitHub ]]; then
        ci=$(github_ci "$repo" "$head" 2>/dev/null || echo "unreadable")
      else
        ci=$(forgejo_ci "$repo" "$head" 2>/dev/null || echo "unreadable")
      fi
      case $ci in
        green) ;;
        none) [[ ${ALLOW_NO_CI:-0} == 1 ]] || state+=("$forge CI none (no checks on the commit)") ;;
        *) state+=("$forge CI $ci") ;;
      esac
    done
    if [[ ${#state[@]} -eq 0 ]]; then
      printf '%-20s %-12s %-12s %s\n' "$repo" "$branch" "${head:0:12}" "ok"
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
  local version=$1 confirm=${2:-} tag=v$1 repo branch dir i step
  local -a repos=() done_repos=()
  cmd_preflight "$version"
  if [[ $confirm != --yes ]]; then
    echo "dry run: rerun with --yes to create $tag in every repository above and push it"
    return 0
  fi
  while read -r repo branch; do repos+=("$repo"); done < <(fleet)
  for i in "${!repos[@]}"; do
    repo=${repos[$i]}
    dir=$(checkout_of "$repo")
    step=""
    if ! git -C "$dir" tag -a "$tag" -m "gRParse fleet $version"; then
      step="creating the tag"
    elif ! git -C "$dir" push -q "$forgejo" "refs/tags/$tag"; then
      step="pushing to $forgejo"
    elif ! git -C "$dir" push -q "$github" "refs/tags/$tag"; then
      step="pushing to $github (it is on $forgejo)"
    fi
    if [[ -n $step ]]; then
      echo "release: $repo failed at $step" >&2
      echo "release: tagged and pushed to both: ${done_repos[*]:-none}" >&2
      echo "release: not tagged: ${repos[*]:$i}" >&2
      exit 1
    fi
    done_repos+=("$repo")
    echo "$repo: $tag pushed to $forgejo and $github"
  done
  echo "tagged; the publish runs and then the release workflow take it from here"
}

[[ $# -ge 2 ]] || usage
command=$1 version=$2
shift 2
[[ $version =~ $semver ]] || die "'$version' is not a semantic version such as 0.3.0 or 0.3.0-rc.1"
[[ -f $fleet_file ]] || die "no fleet file at $fleet_file"
case $command in
  bump) [[ $# -le 1 ]] || usage; cmd_bump "$version" "${1:-}" ;;
  preflight) [[ $# -eq 0 ]] || usage; cmd_preflight "$version" ;;
  tag) [[ $# -le 1 ]] || usage; cmd_tag "$version" "${1:-}" ;;
  *) usage ;;
esac
