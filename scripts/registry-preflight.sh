#!/usr/bin/env bash
# Proves the Docker Hub credentials can do what a publish run needs, before
# any leg spends an hour building.
#
#   DOCKER_USER=... DOCKER_TOKEN=... scripts/registry-preflight.sh \
#       --push pipestreamai/grparse --pull library/ubuntu --pull nvidia/cuda
#
# For every scope it asks Docker Hub's token service (the same exchange
# `docker login` and every push perform) for a registry token and reads the
# access the token was actually granted. A token without write access, or an
# organization token without "Read public repositories", still logs in; the
# token service then grants fewer actions than were asked for, and the push
# or the public base pull fails only at the end of the build. Here the
# missing action fails this script at once and names the repository.
# Exit 0: every scope granted; 1: a scope missing or the login refused.
set -euo pipefail

: "${DOCKER_USER:?DOCKER_USER is not set}"
: "${DOCKER_TOKEN:?DOCKER_TOKEN is not set}"

scopes=()
while [ $# -gt 0 ]; do
  case "$1" in
    --push | --pull)
      [ $# -ge 2 ] || { echo "registry-preflight: $1 needs a repository" >&2; exit 2; }
      if [ "$1" = --push ]; then scopes+=("$2:pull,push"); else scopes+=("$2:pull"); fi
      shift 2 ;;
    *) echo "registry-preflight: unknown argument $1" >&2; exit 2 ;;
  esac
done
[ ${#scopes[@]} -gt 0 ] || { echo "registry-preflight: no --push or --pull scope given" >&2; exit 2; }

# The token response goes to a private temporary file, and the credentials
# reach curl as a header on stdin, never on its command line where any
# process on the runner could read them.
response=$(mktemp)
trap 'rm -f "$response"' EXIT
basic=$(printf '%s:%s' "$DOCKER_USER" "$DOCKER_TOKEN" | base64 | tr -d '\n')

failed=0
for scope in "${scopes[@]}"; do
  repo=${scope%%:*}
  wanted=${scope#*:}
  status=$(printf 'Authorization: Basic %s\n' "$basic" | curl -sS -o "$response" -w '%{http_code}' \
    -H @- \
    "https://auth.docker.io/token?service=registry.docker.io&scope=repository:$repo:$wanted")
  if [ "$status" != 200 ]; then
    echo "registry-preflight: Docker Hub refused the login for $repo (HTTP $status): check DOCKER_USER and DOCKER_TOKEN" >&2
    failed=1
    continue
  fi
  granted=$(python3 - "$repo" "$response" <<'PY'
import base64, json, sys
token = json.load(open(sys.argv[2]))["token"]
payload = token.split(".")[1]
payload += "=" * (-len(payload) % 4)
claims = json.loads(base64.urlsafe_b64decode(payload))
actions = set()
for entry in claims.get("access") or []:
    if entry.get("type") == "repository" and entry.get("name") == sys.argv[1]:
        actions.update(entry.get("actions") or [])
print(",".join(sorted(actions)))
PY
)
  missing=
  for action in ${wanted//,/ }; do
    case ",$granted," in
      *",$action,"*) ;;
      *) missing="$missing $action" ;;
    esac
  done
  if [ -n "$missing" ]; then
    echo "registry-preflight: the token was granted '${granted:-nothing}' on $repo, missing:$missing" >&2
    failed=1
  else
    echo "registry-preflight: $repo: $wanted granted"
  fi
done
exit "$failed"
