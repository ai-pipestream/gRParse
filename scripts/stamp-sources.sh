#!/bin/sh
# Content stamps for first-party sources inside a cache-mounted build tree.
#
# A BuildKit cache mount outlives the COPY that refreshes the sources, and
# COPY does not carry mtimes that ninja can compare with the objects it kept
# from an earlier build. An object compiled from an older version of a file
# can therefore look newer than the file, ninja skips the recompile, and the
# in-build ctest passes on stale code. The proto stamp in the Dockerfiles
# already handles the generated tree; this script does the same per file for
# everything ninja compiles from this repository.
#
#   sh scripts/stamp-sources.sh <build dir>
#
# It writes a manifest of "sha256  path" lines for every first-party source,
# header, test, patch and CMake file, compares each line with the manifest
# kept from the previous build under <build dir>/.src-sums, and touches every
# file whose line is new (changed content or a new file), so ninja sees it
# as newer than any object. Unchanged files keep their objects. Dependencies
# fetched into _deps are not first-party and are not stamped.
set -eu

build_dir="${1:?build dir}"
manifest="$build_dir/.src-sums"
fresh="$build_dir/.src-sums.next"

# The list mirrors what CMakeLists.txt compiles or configures from the tree.
find src include tests compat patches cmake CMakeLists.txt 2>/dev/null \
    -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.h' -o -name '*.hpp' \
               -o -name '*.txt' -o -name '*.cmake' -o -name '*.patch' -o -name '*.in' \) \
  | LC_ALL=C sort \
  | xargs -r sha256sum > "$fresh"

touch "$manifest"
changed=0
while IFS= read -r line; do
  if ! grep -qxF "$line" "$manifest"; then
    path=${line#*  }
    touch "$path"
    changed=$((changed + 1))
    echo "stamp: changed $path"
  fi
done < "$fresh"

mv "$fresh" "$manifest"
echo "stamp: $changed file(s) newer than their objects, $(wc -l < "$manifest") tracked"
