#!/usr/bin/env bash
# Exercises scripts/release.sh against throwaway repositories: a copy of
# this checkout's CMake version line as gRParse, one sibling, and two bare
# remotes per repository standing in for Forgejo and GitHub. gh is a fake
# that reports the CI state named in $work/ci. No network.
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
cat "$work/ci"
FAKE
chmod +x "$work/bin/gh"
echo green >"$work/ci"

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
expect_fail "preflight before the bump" release preflight 0.3.0
grep -q "not 0.3.0" "$work/out"

release bump 0.3.0 --push >/dev/null
grep -q 'VERSION 0.3.0' "$work/fleet/gRParse/CMakeLists.txt"
[[ $(git -C "$work/remotes/gRParse-github.git" log -1 --format=%s main) == "Release 0.3.0" ]]

echo "failed: build=failure" >"$work/ci"
expect_fail "preflight with red CI" release preflight 0.3.0
grep -q "CI failed: build=failure" "$work/out"
echo green >"$work/ci"

echo local >"$work/fleet/grpc-sib/README" && git -C "$work/fleet/grpc-sib" commit -qam ahead
expect_fail "preflight with an unpushed commit" release preflight 0.3.0
grep -q "origin/master is" "$work/out"
git -C "$work/fleet/grpc-sib" push -q origin master && git -C "$work/fleet/grpc-sib" push -q github master

release preflight 0.3.0 >"$work/out"
grep -q "every repository is ready for v0.3.0" "$work/out"

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
