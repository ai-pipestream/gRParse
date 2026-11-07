#!/bin/sh
# Fails an image build whose ctest skipped a test it was not expected to
# skip (#247). A skip (exit 77) is a pass to ctest, so "100% tests passed"
# says nothing about a test that skipped by accident: a missing fixture, a
# mis-set environment variable. The expected skips come from the tier
# labels (cmake/TestTiers.cmake), never from hand-kept test names: a build
# without models may skip `models` tests, one without emulators `emulators`.
#
#   sh scripts/check-skips.sh <ctest --output-junit file> <allowed tiers, comma list>
#   sh scripts/check-skips.sh /build/ctest-junit.xml models,emulators
#
# Exit 1 names every skipped test that carries none of the allowed tiers.
set -eu

junit=${1:?usage: check-skips.sh <junit.xml> <allowed tiers>}
allowed=${2-}
[ -f "$junit" ] || { echo "check-skips: no $junit" >&2; exit 1; }

# One line per skipped test: "<name> <comma labels>". ctest writes each
# testcase's <skipped/> element before its cmake_labels property.
skips=$(awk '
  /<testcase / {
    match($0, /name="[^"]*"/); name = substr($0, RSTART + 6, RLENGTH - 7)
    skipped = 0; labels = ""
  }
  /<skipped/ { skipped = 1 }
  /name="cmake_labels"/ {
    match($0, /value="[^"]*"/); labels = substr($0, RSTART + 7, RLENGTH - 8)
  }
  /<\/testcase>/ { if (skipped) { gsub(/;/, ",", labels); print name, labels } }
' "$junit")

status=0
count=0
unexpected=""
old_ifs=$IFS
while IFS=' ' read -r name labels; do
  [ -n "$name" ] || continue
  count=$((count + 1))
  ok=false
  IFS=,
  for tier in $allowed; do
    for label in $labels; do
      [ "$label" = "$tier" ] && ok=true
    done
  done
  IFS=$old_ifs
  if [ "$ok" != true ]; then
    unexpected="$unexpected $name"
    echo "check-skips: $name skipped, but its tiers [$labels] are not among the allowed skips [$allowed]" >&2
    status=1
  fi
done <<EOS
$skips
EOS

if [ "$status" -eq 0 ]; then
  echo "check-skips: $count skipped, all in allowed tiers [$allowed]"
else
  echo "check-skips: unexpected skips:$unexpected" >&2
fi
exit "$status"
