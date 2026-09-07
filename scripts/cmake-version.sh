#!/usr/bin/env bash
# Prints the project version from the project() line of CMakeLists.txt, the
# one place a release bumps (docs/RELEASING.md). The served GetServiceInfo
# version and the bundle manifest's generator string both derive from it at
# compile time, so this is the version an image built from this tree reports.
#
# With --expect VERSION the script also fails unless that string is the same
# version, which is how the publish workflow refuses to tag an image
# :<version> that would serve a different one: a `v<version>` git tag or a
# dispatch input has to agree with the source it tags. An empty --expect is
# a plain print (a main push publishes only :latest and has no version).
set -euo pipefail

usage() {
  echo "Usage: $0 [--expect VERSION]" >&2
  exit 64
}

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
version=$(sed -n 's/^project(gRParse VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
if [[ -z "$version" ]]; then
  echo "no 'project(gRParse VERSION x.y.z ...)' line in $root/CMakeLists.txt" >&2
  exit 1
fi

expect=
case "${1:-}" in
  "") ;;
  --expect) [[ $# -eq 2 ]] || usage; expect=$2 ;;
  *) usage ;;
esac

if [[ -n "$expect" && "$expect" != "$version" ]]; then
  echo "requested version '$expect' does not match the CMakeLists.txt project version '$version'" >&2
  echo "bump project(gRParse VERSION ...) first, then tag v$expect (docs/RELEASING.md)" >&2
  exit 1
fi
echo "$version"
