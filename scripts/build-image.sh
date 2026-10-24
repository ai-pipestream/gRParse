#!/usr/bin/env bash
# Builds a gRParse image from this checkout with its commit stamped on it.
#
#   scripts/build-image.sh [cuda|cpu|openvino] [docker build args...]
#   scripts/build-image.sh cpu -t grparse-local-cpu
#   scripts/build-image.sh cpu --build-arg GRPARSE_BUILD_CACHE_SCOPE=-myfeature
#
# Runs `docker build` with the flavor's Dockerfile (Dockerfile, Dockerfile.cpu,
# Dockerfile.openvino; cuda is the default) on this checkout's absolute path,
# so a shell whose cwd drifted into another worktree still builds this tree,
# and passes
#   GRPARSE_REVISION    scripts/revision.sh: HEAD, -dirty with uncommitted
#                       changes, empty outside git. It becomes the image's
#                       org.opencontainers.image.revision label, which
#                       eval/corpus records without --grparse-commit.
#   GRPARSE_BUILD_JOBS  scripts/build-jobs.sh: one job per core, at most 8.
# Every other argument goes to `docker build` after these, so it can tag
# (-t), seed (--build-arg GRPARSE_DEPS_IMAGE=...), or override either value.
# The default tag is grparse-local-<flavor>. `docker compose build` reads the
# same GRPARSE_REVISION from the environment:
#   GRPARSE_REVISION=$(scripts/revision.sh) docker compose build
#
# The exit code is docker build's.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

flavor=cuda
case "${1:-}" in
  cuda|cpu|openvino) flavor=$1; shift ;;
  -h|--help) sed -n '2,23p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
esac
case "$flavor" in
  cuda) dockerfile=Dockerfile ;;
  *) dockerfile=Dockerfile.$flavor ;;
esac

revision=$(sh "$ROOT/scripts/revision.sh")
echo "[build-image] $flavor from $ROOT at ${revision:-<no git revision>}" >&2
exec docker build --progress=plain -f "$ROOT/$dockerfile" \
  --build-arg GRPARSE_REVISION="$revision" \
  --build-arg GRPARSE_BUILD_JOBS="$(sh "$ROOT/scripts/build-jobs.sh")" \
  -t "grparse-local-$flavor" "$@" "$ROOT"
