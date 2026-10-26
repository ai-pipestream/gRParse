#!/usr/bin/env bash
# Exercises scripts/release.sh against throwaway repositories: a copy of
# this checkout's CMake version line as gRParse, one sibling, and two bare
# remotes per repository standing in for Forgejo and GitHub. gh is a fake
# that prints the GitHub check runs in $work/ci (name, status, conclusion
# per line), curl a fake Forgejo API serving $work/forgejo-<page>.json.
# No network.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/fleet"
export GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.invalid
export GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.invalid
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1

cat >"$work/bin/gh" <<FAKE
#!/usr/bin/env bash
echo "\$*" >>"$work/gh-args"
cat "$work/ci"
FAKE
cat >"$work/bin/curl" <<FAKE
#!/usr/bin/env bash
echo "\$*" >>"$work/curl-args"
url=\${*: -1}
cat "$work/forgejo-\${url##*page=}.json"
FAKE
chmod +x "$work/bin/gh" "$work/bin/curl"
export FORGEJO_TOKEN=release-test-token
ci() { printf '%s\n' "$@" >"$work/ci"; }
forgejo() { # page total status...
  local page=$1 total=$2; shift 2
  local IFS=,
  local -a items=()
  local s
  for s in "$@"; do items+=("{\"context\":\"ci / ${s%%=*}\",\"status\":\"${s#*=}\"}"); done
  echo "{\"state\":\"x\",\"total_count\":$total,\"statuses\":[${items[*]}]}" >"$work/forgejo-$page.json"
}
green() { ci $'build\tcompleted\tsuccess'; forgejo 1 1 build=success; }
green

make_repo() { # name branch
  local dir=$work/fleet/$1
  git init -q -b "$2" "$dir"
  git init -q --bare "$work/remotes/$1-forgejo.git"
  git init -q --bare "$work/remotes/$1-github.git"
  git -C "$dir" remote add origin "$work/remotes/$1-forgejo.git"
  git -C "$dir" remote add github "$work/remotes/$1-github.git"
}

make_repo gRParse main
mkdir -p "$work/fleet/gRParse/scripts" "$work/fleet/gRParse/release"
cp "$root/scripts/release.sh" "$root/scripts/cmake-version.sh" "$work/fleet/gRParse/scripts/"
echo 'project(gRParse VERSION 0.2.1 LANGUAGES CXX C)' >"$work/fleet/gRParse/CMakeLists.txt"
printf 'gRParse main grparse:{v}\ngrpc-sib master grpc-sib:{v}\n' >"$work/fleet/gRParse/release/fleet.tsv"
git -C "$work/fleet/gRParse" add -A && git -C "$work/fleet/gRParse" commit -qm init
make_repo grpc-sib master
echo sib >"$work/fleet/grpc-sib/README" && git -C "$work/fleet/grpc-sib" add -A && git -C "$work/fleet/grpc-sib" commit -qm init
for r in gRParse:main grpc-sib:master; do
  git -C "$work/fleet/${r%%:*}" push -q origin "${r#*:}"
  git -C "$work/fleet/${r%%:*}" push -q github "${r#*:}"
done

release() { PATH="$work/bin:$PATH" "$work/fleet/gRParse/scripts/release.sh" "$@"; }
expect_fail() { # description command...
  local what=$1; shift
  if "$@" >"$work/out" 2>&1; then echo "release-test: $what unexpectedly passed" >&2; cat "$work/out" >&2; exit 1; fi
}

expect_fail "a bad version" release preflight 0.3
expect_fail "a bad prerelease suffix" release preflight 0.3.0-
expect_fail "preflight before the bump" release preflight 0.3.0
grep -q "not 0.3.0" "$work/out"
expect_fail "a prerelease before the bump" release preflight 0.3.0-rc.1
grep -q "not 0.3.0" "$work/out"

release bump 0.3.0 --push >/dev/null
grep -q 'VERSION 0.3.0' "$work/fleet/gRParse/CMakeLists.txt"
[[ $(git -C "$work/remotes/gRParse-github.git" log -1 --format=%s main) == "Release 0.3.0" ]]

expect_fail "bumping to the version it already has" release bump 0.3.0
grep -q "already 0.3.0" "$work/out"

ci $'build\tcompleted\tfailure' $'lint\tcompleted\tsuccess'
expect_fail "preflight with red GitHub CI" release preflight 0.3.0
grep -q "GitHub CI failed: build=failure" "$work/out"
ci $'build\tin_progress\tnone'
expect_fail "preflight with pending GitHub CI" release preflight 0.3.0
grep -q "GitHub CI pending: build" "$work/out"
ci $'build\tcompleted\tcancelled'
expect_fail "preflight with cancelled GitHub CI" release preflight 0.3.0
grep -q "GitHub CI failed: build=cancelled" "$work/out"
ci
expect_fail "preflight with no GitHub check runs" release preflight 0.3.0
grep -q "GitHub CI none" "$work/out"
ALLOW_NO_CI=1 release preflight 0.3.0 >"$work/out"
grep -q "every repository is ready for v0.3.0" "$work/out"
: >"$work/gh-args"
ci $'build\tcompleted\tsuccess' $'seed\tcompleted\tskipped' $'report\tcompleted\tneutral'
release preflight 0.3.0 >"$work/out"
grep -q "every repository is ready for v0.3.0" "$work/out"
grep -q -- "--paginate" "$work/gh-args"

forgejo 1 2 build=success
forgejo 2 2 image=failure
expect_fail "preflight with red Forgejo CI on a second page" release preflight 0.3.0
grep -q "Forgejo CI failed: ci / image=failure" "$work/out"
forgejo 1 2 build=success seed=skipped
release preflight 0.3.0 >"$work/out"
forgejo 1 1 build=pending
expect_fail "preflight with pending Forgejo CI" release preflight 0.3.0
grep -q "Forgejo CI pending: ci / build" "$work/out"
forgejo 1 0
expect_fail "preflight with no Forgejo statuses" release preflight 0.3.0
grep -q "Forgejo CI none" "$work/out"
if grep -q "$FORGEJO_TOKEN" "$work/curl-args"; then
  echo "release-test: the Forgejo token was on curl's command line" >&2; exit 1
fi
green

echo local >"$work/fleet/grpc-sib/README" && git -C "$work/fleet/grpc-sib" commit -qam ahead
expect_fail "preflight with an unpushed commit" release preflight 0.3.0
grep -q "origin/master is" "$work/out"
git -C "$work/fleet/grpc-sib" push -q origin master && git -C "$work/fleet/grpc-sib" push -q github master

release preflight 0.3.0 >"$work/out"
grep -q "every repository is ready for v0.3.0" "$work/out"

release preflight 0.3.0-rc.1 >"$work/out"
grep -q "every repository is ready for v0.3.0-rc.1" "$work/out"
printf '#!/bin/sh\nexit 1\n' >"$work/remotes/grpc-sib-github.git/hooks/pre-receive"
chmod +x "$work/remotes/grpc-sib-github.git/hooks/pre-receive"
expect_fail "a tag push that fails partway" release tag 0.3.0-rc.1 --yes
grep -q "grpc-sib failed at pushing to github (it is on origin)" "$work/out"
grep -q "tagged and pushed to both: gRParse" "$work/out"
grep -q "not tagged: grpc-sib" "$work/out"
[[ $(git -C "$work/remotes/gRParse-github.git" cat-file -t v0.3.0-rc.1) == tag ]]
rm "$work/remotes/grpc-sib-github.git/hooks/pre-receive"

release tag 0.3.0 >"$work/out"
grep -q "dry run" "$work/out"
if git -C "$work/remotes/grpc-sib-github.git" rev-parse -q --verify refs/tags/v0.3.0 >/dev/null; then
  echo "release-test: the dry run pushed a tag" >&2; exit 1
fi

release tag 0.3.0 --yes >/dev/null
for r in gRParse grpc-sib; do
  for f in forgejo github; do
    [[ $(git -C "$work/remotes/$r-$f.git" cat-file -t v0.3.0) == tag ]] \
      || { echo "release-test: $r-$f has no annotated v0.3.0" >&2; exit 1; }
  done
done
expect_fail "tagging the same version twice" release preflight 0.3.0
grep -q "v0.3.0 already on origin" "$work/out"

echo "release-test: OK"
