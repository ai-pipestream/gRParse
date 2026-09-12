#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
script="$root/scripts/compose-preflight.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin"

cat >"$work/bin/docker" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1 $2" == "compose version" || "$1 $2" == "compose -f" ]] || exit 1
exit 0
EOF
chmod +x "$work/bin/docker"

PATH="$work/bin:$PATH" "$script" --preflight >/dev/null
if PATH="$work/bin:$PATH" "$script" --runtime >/dev/null 2>&1; then
  echo "runtime without a target unexpectedly passed" >&2
  exit 1
fi
if PATH="$work/bin:$PATH" "$script" --unknown >/dev/null 2>&1; then
  echo "unknown argument unexpectedly passed" >&2
  exit 1
fi
echo "compose-preflight-test: OK"
