#!/usr/bin/env bash
# Every service of compose.stack.yaml must come back after a crash or a host
# reboot: each one carries `restart: unless-stopped`. The one-shot model
# loader of compose.stack.models.yaml must not: it carries `restart: "no"`.
#
#   scripts/test/stack-restart-policy-test.sh [stack-file] [models-file]
#
# Reads the files as text (no docker, no YAML library), so it runs inside the
# image builds. The two arguments exist for the self-check at the end, which
# proves the reader fails on a service without a policy and on a loader that
# restarts.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
stack=${1:-$root/compose.stack.yaml}
models=${2:-$root/compose.stack.models.yaml}

# Prints "<service> <policy>" for every service of a compose file, with "-"
# for a service that sets none. A service is a two-space-indented key under
# the top-level `services:` block; its policy is a four-space `restart:` key.
policies() {
  awk '
    function flush() { if (name != "") print name, policy }
    /^services:[ \t]*$/ { inside = 1; next }
    /^[^ #\t]/ { flush(); name = ""; inside = 0 }
    inside && /^  [A-Za-z0-9_-]+:[ \t]*$/ {
      flush(); name = $1; sub(/:$/, "", name); policy = "-"; next
    }
    inside && name != "" && /^    restart:/ {
      policy = $2; gsub(/"/, "", policy)
    }
    END { flush() }
  ' "$1"
}

check() {
  local stack_file=$1 models_file=$2 failed=0 count=0 name policy
  while read -r name policy; do
    count=$((count + 1))
    if [ "$policy" != unless-stopped ]; then
      echo "stack-restart-policy: $stack_file: service $name has restart '$policy', wanted unless-stopped" >&2
      failed=1
    fi
  done < <(policies "$stack_file")
  if [ "$count" -eq 0 ]; then
    echo "stack-restart-policy: $stack_file: no services found" >&2
    failed=1
  fi
  local loader
  loader=$(policies "$models_file" | awk '$1 == "models" { print $2 }')
  if [ "$loader" != no ]; then
    echo "stack-restart-policy: $models_file: models has restart '${loader:-missing}', wanted no" >&2
    failed=1
  fi
  return "$failed"
}

check "$stack" "$models"

# Self-check: the reader must fail on each broken input.
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
grep -v '^    restart: unless-stopped$' "$stack" > "$work/no-policy.yaml"
if check "$work/no-policy.yaml" "$models" 2>/dev/null; then
  echo "stack-restart-policy: a stack file without policies passed" >&2
  exit 1
fi
sed 's/^    restart: "no"$/    restart: unless-stopped/' "$models" > "$work/loader-restarts.yaml"
if check "$stack" "$work/loader-restarts.yaml" 2>/dev/null; then
  echo "stack-restart-policy: a model loader that restarts passed" >&2
  exit 1
fi
echo "stack-restart-policy-test: OK ($(policies "$stack" | wc -l) services)"
