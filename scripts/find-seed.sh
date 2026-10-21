#!/bin/sh
# Names the deps seed an image build should start from.
#
#   sh scripts/find-seed.sh <Dockerfile> <flavor> <arch>
#
# Prints "image=<ref>" for GitHub's step output: the published seed
# pipestreamai/grparse-deps:<flavor>-<arch>-<hash> (scripts/deps-hash.sh) when
# the registry has it, otherwise "image=deps", the Dockerfile's own deps
# stage, which builds the dependencies inline exactly as an unseeded build
# always has. A seed is missing when this change touched a seed input and
# publish-deps.yml has not published that hash yet; the build is then slow,
# not different. The reason goes to stderr for the job log.
set -eu

dockerfile="${1:?Dockerfile}"
flavor="${2:?flavor}"
arch="${3:?arch}"
ref="docker.io/pipestreamai/grparse-deps:$flavor-$arch-$(sh scripts/deps-hash.sh "$dockerfile" "$arch")"

if docker manifest inspect "$ref" >/dev/null 2>&1; then
  echo "find-seed: building from $ref" >&2
  echo "image=$ref"
else
  echo "find-seed: $ref is not published; building the dependencies inline" >&2
  echo "image=deps"
fi
