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

# The digest of the platform manifest for $arch in `docker manifest inspect -v`
# output on stdin. That output is a list for an image index (one entry per
# platform, attestation entries carry platform "unknown") and a single object
# for a plain manifest, which has no platform field. Fails when no entry is
# for $arch.
platform_digest() {
  python3 -c '
import json, sys
arch = sys.argv[1]
data = json.load(sys.stdin)
entries = data if isinstance(data, list) else [data]
for entry in entries:
    descriptor = entry["Descriptor"]
    platform = descriptor.get("platform")
    if platform is None or platform["architecture"] == arch:
        print(descriptor["digest"])
        sys.exit(0)
sys.exit(f"find-seed: no {arch} manifest in the index")
' "$arch"
}

if docker image inspect "$ref" >/dev/null 2>&1; then
  # A pulled seed keeps the digest it was pulled by. A seed built on this
  # host and never pushed has none; it is named by tag, and that tag lives
  # only in this host's own image store.
  local_digest=$(docker image inspect --format '{{range .RepoDigests}}{{println .}}{{end}}' "$ref" |
    sed -n 's|^.*grparse-deps@\(sha256:[0-9a-f]*\)$|\1|p' | head -n 1)
  if [ -n "$local_digest" ]; then
    echo "find-seed: building from $ref@$local_digest, already local" >&2
    echo "image=$ref@$local_digest"
  else
    echo "find-seed: building from $ref, already local and built here" >&2
    echo "image=$ref"
  fi
elif [ "$no_pull" = 1 ]; then
  echo "find-seed: $ref is not local and --no-pull is set; building on the deps stage" >&2
  echo "image=deps"
elif manifest=$(docker manifest inspect -v "$ref" 2>/dev/null); then
  digest=$(printf '%s' "$manifest" | platform_digest)
  echo "find-seed: building from $ref@$digest" >&2
  echo "image=$ref@$digest"
else
  echo "find-seed: $ref is not published; building the dependencies inline" >&2
  echo "image=deps"
fi
