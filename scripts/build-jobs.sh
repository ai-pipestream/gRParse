#!/bin/sh
# Prints the compile parallelism for an image build on this runner: one job
# per core, at most 8. The Dockerfiles default to 2, which left half of a
# 4-core hosted runner idle (GitHub image jobs took 48 to 60 min at -j2,
# run 39362730667). The cap keeps a large self-hosted runner, whose core
# count nproc may report beyond its job's memory share, from running more
# gRPC compiles at once than its memory holds.
set -eu
jobs=$(nproc)
[ "$jobs" -le 8 ] || jobs=8
echo "$jobs"
