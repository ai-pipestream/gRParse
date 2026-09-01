#!/usr/bin/env bash
# Bring the parser stack up and run the Playwright suite against it.
#
#   scripts/stack-e2e.sh                # GPU stack, project parse-stack
#   NO_GPU=1 scripts/stack-e2e.sh       # + compose.stack.cpu.yaml
#   INTEL=1 scripts/stack-e2e.sh        # + compose.stack.openvino.yaml
#
# Environment:
#   E2E_PROJECT        compose project name (default parse-stack, the name
#                      compose.stack.yaml declares, so an already-running
#                      stack is reused rather than duplicated)
#   E2E_BUILD=1        pass --build to `up`
#   E2E_DOWN=1         tear the project down after the run
#   E2E_WAIT_SECONDS   `up --wait` timeout (default 600)
#   E2E_WORKERS        Playwright workers (default 1; see e2e/playwright.config.ts)
#
# The exit code is Playwright's exit code; nothing here greps output.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit 0
fi

PROJECT=${E2E_PROJECT:-parse-stack}
files=(-f compose.stack.yaml)
if [ "${NO_GPU:-0}" = "1" ]; then files+=(-f compose.stack.cpu.yaml); fi
if [ "${INTEL:-0}" = "1" ]; then files+=(-f compose.stack.openvino.yaml); fi
files+=(-f compose.stack.e2e.yaml)

E2E_UID=$(id -u)
E2E_GID=$(id -g)
export E2E_UID E2E_GID

compose=(docker compose -p "$PROJECT" "${files[@]}")
stack_profiles=(--profile parsers --profile heavy)

up_args=(up -d --wait --wait-timeout "${E2E_WAIT_SECONDS:-600}")
if [ "${E2E_BUILD:-0}" = "1" ]; then up_args+=(--build); fi

echo "[stack-e2e] project $PROJECT: ${compose[*]} ${stack_profiles[*]} ${up_args[*]}"
"${compose[@]}" "${stack_profiles[@]}" "${up_args[@]}"

echo "[stack-e2e] running the Playwright suite (reports under e2e/out/)"
set +e
"${compose[@]}" --profile e2e run --rm playwright
status=$?
set -e

if [ "${E2E_DOWN:-0}" = "1" ]; then
  "${compose[@]}" "${stack_profiles[@]}" --profile e2e down --remove-orphans
fi

echo "[stack-e2e] playwright exit code $status"
exit "$status"
