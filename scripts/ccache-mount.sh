#!/bin/sh
# Carries an image build's ccache mount across hosted CI runs.
#
#   sh scripts/ccache-mount.sh load <flavor> <arch> <dir>
#   sh scripts/ccache-mount.sh save <flavor> <arch> <dir>
#
# The image Dockerfiles compile the first-party sources through ccache, whose
# directory is the BuildKit cache mount grparse-ccache-<flavor>-<arch>. On a
# persistent Docker daemon (a workstation, Forgejo's runner) that mount
# outlives the build and nothing here is needed. A hosted runner starts with
# an empty builder, so CI restores <dir> from its cache, `load` copies it into
# the mount before the image build, and `save` copies the mount back out into
# <dir> afterwards for CI to store. Both run a one-step build on the current
# builder, the one the image build uses; the mount id is all they share.
# --no-cache would hand the step an empty mount, so a per-call build arg is
# what keeps the step from being answered from the build cache instead.
set -eu

mode="${1:?load or save}"
flavor="${2:?flavor}"
arch="${3:?arch}"
dir="${4:?dir}"
mount="type=cache,id=grparse-ccache-$flavor-$arch,target=/ccache"
run="$(date +%s)-$$"

case "$mode" in
  load)
    mkdir -p "$dir"
    printf '%s\n' 'FROM ubuntu:26.04' 'ARG CCACHE_MOUNT_RUN' \
      "RUN --mount=$mount --mount=type=bind,target=/in cp -a /in/. /ccache/ && du -sh /ccache" \
      | docker buildx build --progress=plain --build-arg CCACHE_MOUNT_RUN="$run" --output type=cacheonly -f - "$dir"
    ;;
  save)
    context=$(mktemp -d)
    trap 'rm -rf "$context"' EXIT
    rm -rf "$dir"
    printf '%s\n' 'FROM ubuntu:26.04 AS copy' 'ARG CCACHE_MOUNT_RUN' \
      "RUN --mount=$mount mkdir /out && cp -a /ccache/. /out/ && du -sh /out" \
      'FROM scratch' 'COPY --from=copy /out/ /' \
      | docker buildx build --progress=plain --build-arg CCACHE_MOUNT_RUN="$run" --output "type=local,dest=$dir" -f - "$context"
    ;;
  *)
    echo "ccache-mount: unknown mode '$mode' (load or save)" >&2
    exit 64
    ;;
esac
