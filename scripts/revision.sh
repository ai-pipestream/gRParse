#!/bin/sh
# Prints the commit a local image build is made from, for the
# GRPARSE_REVISION build arg that every image Dockerfile turns into the
# org.opencontainers.image.revision label (the label eval/corpus records as
# the gRParse commit of a run).
#
#   sh scripts/revision.sh
#
# The full HEAD sha, with -dirty appended when a tracked file differs from
# HEAD, so an image built from uncommitted changes never claims a commit it
# does not match. Outside a git checkout (a source archive, no git) it prints
# nothing and exits 0: the label stays empty and eval/corpus records the
# commit as not recorded instead of guessing.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
sha=$(git -C "$root" rev-parse --verify --quiet HEAD 2>/dev/null) || exit 0
if ! git -C "$root" diff --quiet HEAD -- 2>/dev/null; then
  sha="$sha-dirty"
fi
echo "$sha"
