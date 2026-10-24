#!/bin/sh
# Turns a finished build tree into a deps seed (see scripts/deps-hash.sh).
#
#   sh scripts/prune-seed.sh <build dir> <seed dir>
#
# Moves the tree to <seed dir> and removes what a later commit always
# rebuilds: every first-party object file (everything outside _deps), the
# linked executables at the top of the tree, and the staged and generated
# proto trees with their stamps. What stays is CMake's cache and generated
# build files, ninja's log and dependency database, and the fetched and built
# third-party trees under _deps. A build seeded from it runs CMake and ninja
# as usual: ninja rebuilds every missing output and every output whose command
# changed, and the third-party objects whose sources and commands are
# unchanged are kept. Nothing is skipped that an unseeded build would run;
# the seed only spares rebuilding identical third-party objects.
set -eu

build_dir="${1:?build dir}"
seed_dir="${2:?seed dir}"

mv "$build_dir" "$seed_dir"
find "$seed_dir" \
  \( -path "$seed_dir/_deps" -o -path "$seed_dir/CMakeFiles/miniz.dir" -o -path "$seed_dir/CMakeFiles/rapidocr.dir" \) -prune \
  -o -type f -name '*.o' -exec rm -f {} +
rm -f "$seed_dir"/libgrparse*.a
find "$seed_dir" -maxdepth 1 -type f -perm -u+x -exec rm -f {} +
rm -rf "$seed_dir/proto" "$seed_dir/generated" "$seed_dir/.proto-sum" "$seed_dir/.src-sums"
echo "seed: $(du -sh "$seed_dir" | cut -f1) in $seed_dir"
