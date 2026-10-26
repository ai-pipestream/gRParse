#!/usr/bin/env bash
# Exercises scripts/deps-hash.sh and scripts/find-seed.sh on a throwaway copy
# of the seed inputs. docker is a fake that answers from files in $work, so
# no registry is asked.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail() { echo "seed-scripts-test: $*" >&2; exit 1; }

tree=$work/tree
mkdir -p "$tree/scripts" "$tree/src"
cp -R "$root/cmake" "$root/patches" "$tree/"
cp "$root/CMakeLists.txt" "$root/Dockerfile" "$root/Dockerfile.cpu" "$tree/"
cp "$root/scripts/deps-hash.sh" "$root/scripts/find-seed.sh" "$root/scripts/prune-seed.sh" "$tree/scripts/"
echo 'int main() {}' >"$tree/src/first_party.cpp"
cd "$tree"

hash() { sh scripts/deps-hash.sh Dockerfile.cpu amd64; }

base=$(hash)
[ "$base" = "$(hash)" ] || fail "deps-hash is not deterministic"
[ "$base" != "$(sh scripts/deps-hash.sh Dockerfile.cpu arm64)" ] || fail "arch is not an input"
[ "$base" != "$(sh scripts/deps-hash.sh Dockerfile amd64)" ] || fail "the Dockerfile is not an input"

expect_moves() { # description, edit command
  local description=$1
  shift
  cp -R "$tree" "$work/saved"
  "$@"
  [ "$(hash)" != "$base" ] || fail "$description does not change the hash"
  rm -rf "$tree"
  mv "$work/saved" "$tree"
  cd "$tree"
  [ "$(hash)" = "$base" ] || fail "restoring after $description did not restore the hash"
}

expect_moves "a set() value in CMakeLists.txt" \
  sed -i 's/^set(GRPARSE_ORT_PACKAGE "cuda"/set(GRPARSE_ORT_PACKAGE "cpu"/' CMakeLists.txt
grep -q '^set(GRPARSE_ORT_PACKAGE "cuda"' CMakeLists.txt || fail "fixture lost the GRPARSE_ORT_PACKAGE default"
# shellcheck disable=SC2016 # the inner sh expands it
expect_moves "an edit to cmake/*.cmake" sh -c 'echo "# edit" >>"$(find cmake -name "*.cmake" | head -n 1)"'
expect_moves "an edit to scripts/prune-seed.sh" sh -c 'echo "# edit" >>scripts/prune-seed.sh'
expect_moves "a new file under patches/" sh -c 'echo x >patches/new.patch'

echo 'int other() { return 1; }' >>src/first_party.cpp
[ "$(hash)" = "$base" ] || fail "a first-party source edit changed the hash"

# find-seed.sh against a fake docker. $work/docker/<subcommand> holds what
# the subcommand prints; a missing file is a failed lookup.
mkdir -p "$work/bin" "$work/docker"
cat >"$work/bin/docker" <<FAKE
#!/usr/bin/env bash
case "\$1 \$2" in
  "image inspect")
    [ -f "$work/docker/image" ] || exit 1
    case "\$*" in *--format*) cat "$work/docker/image" ;; esac ;;
  "manifest inspect")
    [ -f "$work/docker/manifest" ] || exit 1
    cat "$work/docker/manifest" ;;
  *) exit 2 ;;
esac
FAKE
chmod +x "$work/bin/docker"
export PATH="$work/bin:$PATH"
ref="docker.io/pipestreamai/grparse-deps:cpu-amd64-$base"
amd=sha256:$(printf a | sha256sum | cut -d' ' -f1)
arm=sha256:$(printf b | sha256sum | cut -d' ' -f1)
att=sha256:$(printf c | sha256sum | cut -d' ' -f1)
seed() { sh scripts/find-seed.sh "$@" Dockerfile.cpu cpu amd64 2>/dev/null; }

[ "$(seed)" = "image=deps" ] || fail "an unpublished seed is not built inline"

cat >"$work/docker/manifest" <<JSON
[
 {"Ref": "$ref@$arm", "Descriptor": {"digest": "$arm", "platform": {"architecture": "arm64", "os": "linux"}}},
 {"Ref": "$ref@$amd", "Descriptor": {"digest": "$amd", "platform": {"architecture": "amd64", "os": "linux"}}},
 {"Ref": "$ref@$att", "Descriptor": {"digest": "$att", "platform": {"architecture": "unknown", "os": "unknown"}}}
]
JSON
[ "$(seed)" = "image=$ref@$amd" ] || fail "an index is not pinned to its amd64 manifest: $(seed)"
[ "$(seed --no-pull)" = "image=deps" ] || fail "--no-pull named a registry seed"

cat >"$work/docker/manifest" <<JSON
{"Ref": "$ref", "Descriptor": {"digest": "$amd"}}
JSON
[ "$(seed)" = "image=$ref@$amd" ] || fail "a single manifest is not pinned to its digest: $(seed)"

cat >"$work/docker/manifest" <<JSON
[{"Ref": "$ref@$arm", "Descriptor": {"digest": "$arm", "platform": {"architecture": "arm64", "os": "linux"}}}]
JSON
if seed >/dev/null; then fail "an index without amd64 was accepted"; fi
rm "$work/docker/manifest"

printf 'pipestreamai/grparse-deps@%s\n' "$amd" >"$work/docker/image"
[ "$(seed --no-pull)" = "image=$ref@$amd" ] || fail "a pulled local seed is not pinned: $(seed --no-pull)"
: >"$work/docker/image"
[ "$(seed --no-pull)" = "image=$ref" ] || fail "a locally built seed is not named by tag: $(seed --no-pull)"

echo "seed-scripts-test: OK"
