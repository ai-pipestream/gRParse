#!/bin/sh
# Content hash of what a gRParse deps seed image is built from.
#
#   sh scripts/deps-hash.sh <Dockerfile> <arch>
#
# A seed image (the `seed` stage of Dockerfile, Dockerfile.cpu or
# Dockerfile.openvino) is that Dockerfile's toolchain plus one finished
# /build tree with its third-party objects kept and first-party objects
# removed. It is published as pipestreamai/grparse-deps:<flavor>-<arch>-<hash>
# and a later build seeds an empty /build from it. The hash covers every input
# that could make a seeded build differ from an unseeded one:
#   - the Dockerfile up to its "grparse-deps: end of seed inputs" marker
#     (base image, apt packages, TensorRT, OpenCV, the OpenVINO SDK, the
#     seed stage itself),
#   - the build stage's cache-mount id, which encodes the ABI-sensitive
#     dependency versions and is bumped by hand when they change,
#   - every FetchContent declaration line in CMakeLists.txt and cmake/
#     (repository, tag, URL, hash): FetchContent repopulates a changed
#     declaration by itself, but a seed that predates it would rebuild it
#     every run,
#   - every file under patches/: FetchContent does not reapply a changed
#     patch to an already populated tree,
#   - the target architecture.
# Anything else (first-party sources, protos, other CMake changes) is rebuilt
# by ninja and the content stamps in every build, seeded or not.
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
  grep -F 'type=cache,id=' "$dockerfile"
  grep -hE '^[[:space:]]*(FetchContent_Declare|GIT_REPOSITORY|GIT_TAG|URL|URL_HASH)\b' \
    CMakeLists.txt cmake/*.cmake
  find patches -type f | LC_ALL=C sort | xargs -r sha256sum
} | sha256sum | cut -c1-16
