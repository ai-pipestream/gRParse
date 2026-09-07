#!/bin/sh
# Stages the shared-library closure of a set of ELF files into one directory
# so an image's runtime stage can sit on a minimal base: no package manager,
# no ldconfig, nothing but glibc. The Dockerfiles run this in the build stage
# and COPY the directory onto the loader path (LD_LIBRARY_PATH) of the
# runtime stage.
#
#   scripts/stage-runtime-closure.sh OUT_DIR FILE...
#
# FILE is an executable, or a shared library the program dlopens at run time
# (an ldd walk of the executable alone never sees those). For every input the
# script walks `ldd` and copies each resolved library that is not part of
# glibc into OUT_DIR, dereferencing symlinks so the copy carries the SONAME
# file name the loader asks for. glibc stays with the base: the loader and
# its libraries are inseparable. STAGE_KEEP_ON_BASE names, as an extended
# regex on the resolved path, one more family the base is expected to own
# (for example '^/usr/local/cuda' on a CUDA runtime image).
#
# The walk fails loudly on the mistakes that have shipped before:
#   - an input whose dependency is unresolved ("not found") in the build
#     stage, which would only surface as a loader error at deploy time;
#   - two different source paths that would land on the same file name in
#     OUT_DIR, where the second copy silently replaces the first.
# LD_LIBRARY_PATH is inherited, so the caller points it at the vendored
# library directories the inputs were linked against.
set -eu

usage() {
  echo "usage: $0 OUT_DIR FILE..." >&2
  exit 64
}
[ $# -ge 2 ] || usage
out=$1
shift

glibc_family='/(ld-linux[^/]*|libc|libm|libdl|libpthread|librt|libresolv|libnsl|libutil|libanl|libmvec|libnss_[a-z]+|libthread_db|libBrokenLocale)\.so(\.|$)'
keep_on_base=${STAGE_KEEP_ON_BASE:-}

listing=$(mktemp)
resolved=$(mktemp)
trap 'rm -f "$listing" "$resolved"' EXIT

walk_inputs() {
  for f in "$@"; do
    [ -e "$f" ] || { echo "stage-runtime-closure: no such file: $f" >&2; exit 1; }
    ldd "$f" >> "$listing" 2>&1 || { echo "stage-runtime-closure: ldd failed on $f" >&2; exit 1; }
  done
}

fail_on_unresolved() {
  if grep -q 'not found' "$listing"; then
    echo "stage-runtime-closure: unresolved shared libraries among the inputs:" >&2
    grep 'not found' "$listing" | sort -u >&2
    exit 1
  fi
}

# Resolved paths the runtime stage must carry: everything ldd resolved,
# minus glibc and the family the caller keeps on the base.
select_staged() {
  awk '/=> \// {print $3}' "$listing" | sort -u | while read -r lib; do
    if printf '%s' "$lib" | grep -Eq "$glibc_family"; then continue; fi
    if [ -n "$keep_on_base" ] && printf '%s' "$lib" | grep -Eq "$keep_on_base"; then continue; fi
    printf '%s\n' "$lib"
  done > "$resolved"
}

fail_on_name_collision() {
  dups=$(awk -F/ '{print $NF}' "$resolved" | sort | uniq -d)
  if [ -n "$dups" ]; then
    echo "stage-runtime-closure: one file name resolves from more than one path:" >&2
    for name in $dups; do grep "/$name\$" "$resolved" >&2; done
    exit 1
  fi
}

copy_staged() {
  mkdir -p "$out"
  while read -r lib; do cp -L "$lib" "$out/"; done < "$resolved"
  echo "stage-runtime-closure: staged $(wc -l < "$resolved") libraries into $out"
}

walk_inputs "$@"
fail_on_unresolved
select_staged
fail_on_name_collision
copy_staged
