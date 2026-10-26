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
#   - the build stage's /build cache-mount id, which encodes the
#     ABI-sensitive dependency versions and is bumped by hand when they
#     change (the ccache mount beside it is not an input: ccache keys every
#     object on its preprocessed source, flags and compiler),
#   - every FetchContent_Declare call and every set(GRPARSE_ORT...) call in
#     CMakeLists.txt and cmake/, each whole across its lines (repository,
#     tag, URL, hash, and the ONNX Runtime package, wheel name and checksum
#     that the URLs are built from): FetchContent repopulates a changed
#     declaration by itself, but a seed that predates it would rebuild it
#     every run,
#   - scripts/prune-seed.sh, which decides what the seed tree keeps,
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
  grep -F 'type=cache,id=' "$dockerfile" | grep -F 'target=/build'
  # Each call from its opening line to the parenthesis that closes it, with
  # comments dropped so rewording one does not orphan the published seeds.
  awk '
    { line = $0; sub(/#.*/, "", line) }
    !depth && line ~ /^[[:space:]]*(FetchContent_Declare|set[[:space:]]*\([[:space:]]*GRPARSE_ORT)/ {
      depth = 0.5
    }
    depth && line !~ /^[[:space:]]*$/ {
      print line
      opened = gsub(/\(/, "", line)
      closed = gsub(/\)/, "", line)
      depth = (depth == 0.5 ? 0 : depth) + opened - closed
    }
  ' CMakeLists.txt cmake/*.cmake
  sha256sum scripts/prune-seed.sh
  find patches -type f | LC_ALL=C sort | xargs -r sha256sum
} | sha256sum | cut -c1-16
