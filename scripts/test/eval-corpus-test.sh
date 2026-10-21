#!/usr/bin/env bash
# scripts/eval-corpus.sh --dry-run: the compose and runner commands it would
# issue, without docker or a corpus.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
script="$root/scripts/eval-corpus.sh"

fail() { echo "eval-corpus-test: $*" >&2; exit 1; }

out=$("$script" --corpus /corpus --flavor cpu --models-image --grparse-image local:1 \
  --grparse-commit abc123 --label L --dry-run 2>&1)
grep -q -- '-p grparse-eval -f compose.stack.yaml -f compose.stack.cpu.yaml -f compose.stack.models.yaml -f eval/corpus/compose.eval.yaml' <<<"$out" \
  || fail "compose files wrong: $out"
grep -q -- 'up -d --no-build --pull always grparse ' <<<"$out" || fail "up line wrong"
grep -q -- '--compose-project grparse-eval' <<<"$out" || fail "run.py not told the project"
grep -q -- '--target localhost:50151' <<<"$out" || fail "default target wrong"
grep -q -- '--repeat 2' <<<"$out" || fail "battery repeat missing"
grep -q -- '--grparse-commit abc123' <<<"$out" || fail "commit not passed"
grep -q -- '--baseline-auto' <<<"$out" || fail "auto baseline missing"
grep -q -- ' down --remove-orphans' <<<"$out" || fail "stack not torn down"

out=$("$script" --corpus /corpus --existing parse-stack --baseline none --label L --dry-run 2>&1)
if grep -q 'docker compose' <<<"$out"; then fail "--existing started a stack"; fi
grep -q -- '--target localhost:50051' <<<"$out" || fail "existing target wrong"
grep -q -- '--compose-project parse-stack' <<<"$out" || fail "existing project not recorded"
if grep -q -- '--baseline-auto' <<<"$out"; then fail "--baseline none still compared"; fi

if "$script" --flavor cpu --dry-run >/dev/null 2>&1; then fail "missing --corpus passed"; fi
if "$script" --corpus /c --flavor gpu --dry-run >/dev/null 2>&1; then fail "bad flavor passed"; fi
echo "eval-corpus-test: OK"
