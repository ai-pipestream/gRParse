#!/bin/sh
# Content hash of what a gRParse deps seed image is built from.
#
#   sh scripts/deps-hash.sh <Dockerfile> <arch>
#
# A seed image (the `seed` stage of Dockerfile, Dockerfile.cpu or
# Dockerfile.openvino) is that Dockerfile's toolchain plus one finished
# /build tree with its third-party objects kept and first-party objects
# removed. It is published as pipestreamai/grparse-deps:<flavor>-<arch>-<hash>
# and a later build seeds an empty /build from it. The tag names these inputs,
# so a change to any of them names a new seed:
#   - the Dockerfile up to its "grparse-deps: end of seed inputs" marker
#     (base image, apt packages, TensorRT, OpenCV, the OpenVINO SDK, the
#     seed stage itself),
#   - the build stage's /build cache-mount id, which encodes the
#     ABI-sensitive dependency versions and is bumped by hand when they
#     change (the ccache mount beside it is not an input: ccache keys every
#     object on its preprocessed source, flags and compiler),
#   - all of CMakeLists.txt and cmake/*.cmake, whole files: a FetchContent
#     URL or hash can come from a set() or an option elsewhere in the file,
#     and a non-FORCE cache default stays in the seed's CMakeCache.txt, so
#     any CMake edit names a new seed,
#   - every file under patches/: FetchContent does not reapply a changed
#     patch to an already populated tree,
#   - scripts/prune-seed.sh, which decides what the seed keeps, and this
#     script,
#   - the target architecture.
# First-party sources and protos are not inputs: ninja and the content stamps
# rebuild them in every build, seeded or not.
set -eu

dockerfile="${1:?Dockerfile}"
arch="${2:?arch}"
marker='# grparse-deps: end of seed inputs'

grep -qxF "$marker" "$dockerfile" || {
  echo "deps-hash: $dockerfile has no '$marker' line" >&2
  exit 1
}

{
  echo "arch $arch"
  sed "/^$marker\$/q" "$dockerfile"
  grep -F 'type=cache,id=' "$dockerfile" | grep -F 'target=/build'
  {
    echo CMakeLists.txt
    find cmake -name '*.cmake' -type f
    find patches -type f
    echo scripts/prune-seed.sh
    echo scripts/deps-hash.sh
  } | LC_ALL=C sort | xargs sha256sum
} | sha256sum | cut -c1-16
