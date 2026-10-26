#!/bin/sh
# Names the deps seed an image build should start from.
#
#   sh scripts/find-seed.sh [--no-pull] <Dockerfile> <flavor> <arch>
#
# Prints "image=<ref>" for GitHub's step output: the published seed
# pipestreamai/grparse-deps:<flavor>-<arch>-<hash> (scripts/deps-hash.sh) when
# the registry has it, pinned to the digest of its linux/<arch> manifest
# (<ref>@sha256:...) so a tag rewritten after this step cannot change what
# the build starts from, otherwise "image=deps", the Dockerfile's own deps
# stage, which builds the dependencies inline exactly as an unseeded build
# always has. A seed is missing when this change touched a seed input and
# publish-deps.yml has not published that hash yet; the build is then slow,
# not different. The reason goes to stderr for the job log.
#
# A seed already in the local image store is named without asking the
# registry. --no-pull names nothing that would have to be pulled: a runner
# whose Docker daemon persists between jobs (Forgejo's) keeps the deps stage
# in its build cache and a filled /build in its cache mount, and the seed's
# only use is filling an empty /build, so its ~3.4 GB pull is wasted there
# whenever the tree is already warm. When that cache is cold (a pruned
# builder, a new cache id) the deps stage builds the dependencies once, as an
# unseeded build does.
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
elif manifest=$(docker manifest inspect -v "$ref" 2>/dev/null); then
  # One entry per manifest under the tag; build attestations sit beside the
  # image with an unknown platform, so pick the image by its platform.
  pinned=$(printf '%s' "$manifest" | jq -r --arg arch "$arch" '
    [if type == "array" then .[] else . end
     | select(.Descriptor.platform.os == "linux" and .Descriptor.platform.architecture == $arch)
     | .Ref][0] // empty')
  if [ -z "$pinned" ]; then
    echo "find-seed: $ref has no linux/$arch manifest; building the dependencies inline" >&2
    echo "image=deps"
  else
    echo "find-seed: building from $pinned" >&2
    echo "image=$pinned"
  fi
else
  echo "find-seed: $ref is not published; building the dependencies inline" >&2
  echo "image=deps"
fi
