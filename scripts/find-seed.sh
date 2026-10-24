#!/bin/sh
# Names the deps seed an image build should start from.
#
#   sh scripts/find-seed.sh [--no-pull] <Dockerfile> <flavor> <arch>
#
# Prints "image=<ref>" for GitHub's step output: the published seed resolved to
# its immutable manifest digest
# docker.io/pipestreamai/grparse-deps:<flavor>-<arch>-<hash>@sha256:<digest>
# (scripts/deps-hash.sh) when the registry has it, otherwise "image=deps", the
# Dockerfile's own deps stage, which builds the dependencies inline exactly as
# an unseeded build always has. A seed is missing when this change touched a
# seed input and publish-deps.yml has not published that hash yet; the build is
# then slow, not different. The reason goes to stderr for the job log.
#
# A seed already in the local image store is named without asking the
# registry. --no-pull names nothing that would have to be pulled.
set -eu

no_pull=0
if [ "${1:-}" = --no-pull ]; then
  no_pull=1
  shift
fi
dockerfile="${1:?Dockerfile}"
flavor="${2:?flavor}"
arch="${3:?arch}"
ref="docker.io/pipestreamai/grparse-deps:$flavor-$arch-$(sh scripts/deps-hash.sh "$dockerfile" "$arch")"

if docker image inspect "$ref" >/dev/null 2>&1; then
  echo "find-seed: building from $ref, already local" >&2
  echo "image=$ref"
elif [ "$no_pull" = 1 ]; then
  echo "find-seed: $ref is not local and --no-pull is set; building on the deps stage" >&2
  echo "image=deps"
elif manifest=$(docker manifest inspect -v "$ref" 2>/dev/null) &&
   pinned=$(printf '%s' "$manifest" | python3 -c '
import json, sys
arch = sys.argv[1]
data = json.load(sys.stdin)
entries = data if isinstance(data, list) else [data]
for entry in entries:
    desc = entry.get("Descriptor") or {}
    plat = desc.get("platform") or {}
    digest = desc.get("digest") or ""
    if digest and (plat.get("architecture") in (None, arch)):
        print(entry.get("Ref") or f"{sys.argv[2]}@{digest}")
        sys.exit(0)
sys.exit(1)
' "$arch" "$ref"); then
  echo "find-seed: building from $pinned" >&2
  echo "image=$pinned"
else
  echo "find-seed: $ref is not published; building the dependencies inline" >&2
  echo "image=deps"
fi
