#!/bin/sh
# Feeds synthetic ctest JUnit files to scripts/check-skips.sh (#247).
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

testcase() {  # name labels skipped(0|1)
  echo "	<testcase name=\"$1\" classname=\"$1\" time=\"0.01\" status=\"$( [ "$3" = 1 ] && echo notrun || echo run)\">"
  [ "$3" = 1 ] && echo '		<skipped message="SKIP_RETURN_CODE=77"/>'
  echo '		<properties>'
  echo "			<property name=\"cmake_labels\" value=\"$2\"/>"
  echo '		</properties>'
  echo '		<system-out></system-out>'
  echo '	</testcase>'
}
{
  echo '<?xml version="1.0" encoding="UTF-8"?>'
  echo '<testsuite name="Linux-c++" tests="4">'
  testcase base64-test "grparse;unit" 0
  testcase layout-engine-test "grparse;models" 1
  testcase storage-connectors-e2e-test "emulators;grparse" 1
  echo '</testsuite>'
} > "$work/expected.xml"
sed 's#</testsuite>##' "$work/expected.xml" > "$work/unexpected.xml"
testcase render-test "grparse;unit" 1 >> "$work/unexpected.xml"
echo '</testsuite>' >> "$work/unexpected.xml"

fail=0
sh "$root/scripts/check-skips.sh" "$work/expected.xml" models,emulators >/dev/null || { echo "FAIL: allowed skips rejected" >&2; fail=1; }
if sh "$root/scripts/check-skips.sh" "$work/expected.xml" emulators >/dev/null 2>&1; then
  echo "FAIL: a models skip passed where only emulators may skip" >&2; fail=1
fi
out=$(sh "$root/scripts/check-skips.sh" "$work/unexpected.xml" models,emulators 2>&1) && { echo "FAIL: a skipped unit test passed" >&2; fail=1; }
echo "$out" | grep -q "render-test" || { echo "FAIL: the unexpected skip is not named" >&2; fail=1; }
if sh "$root/scripts/check-skips.sh" "$work/missing.xml" models >/dev/null 2>&1; then
  echo "FAIL: a missing JUnit file passed" >&2; fail=1
fi
[ "$fail" -eq 0 ] && echo "check-skips-test: ok"
exit "$fail"
