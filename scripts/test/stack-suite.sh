#!/bin/sh
# Runs one compose-stack suite for CTest's `stack` tier (#244), exactly as
# it runs by hand; this wrapper only decides whether it runs.
#
#   GRPARSE_STACK_SUITES=all ctest --test-dir build -L stack
#   GRPARSE_STACK_SUITES=stack-e2e,s3-corpus-eval ctest --preset nightly
#
# A suite runs when GRPARSE_STACK_SUITES (a comma list) names it or says
# `all`, and is skipped (77) otherwise, so a plain `ctest -L grparse` never
# brings a stack up. A suite that was asked for and lacks what it needs
# fails instead of skipping. Each suite's own configuration is the
# environment its script already reads:
#
#   stack-e2e             scripts/stack-e2e.sh (NO_GPU, INTEL, E2E_BUILD, E2E_DOWN,
#                         E2E_WAIT_SECONDS); exit code is Playwright's
#   s3-corpus-eval        eval/s3/run.py (EVAL_S3_*, GRPARSE_TARGET; eval/s3/README.md)
#   ocr-presets-e2e       scripts/test/ocr-presets-e2e-test.sh with
#                         GRPARSE_STACK_IMAGE and GRPARSE_STACK_MODELS_DIR
#   embedding-rpc         scripts/test/embedding-rpc-test.sh (EMBEDDING_RPC_*),
#                         with EMBEDDING_RPC_FIXTURE as its fixture when set
#   compose-preflight     scripts/test/compose-preflight-test.sh
#   observability-compose scripts/test/observability-compose-test.sh
set -eu

suite=${1:?usage: stack-suite.sh <suite>}
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"

wanted=false
old_ifs=$IFS
IFS=,
for name in ${GRPARSE_STACK_SUITES:-}; do
  if [ "$name" = all ] || [ "$name" = "$suite" ]; then wanted=true; fi
done
IFS=$old_ifs
if [ "$wanted" != true ]; then
  echo "stack-suite: $suite not in GRPARSE_STACK_SUITES; skipped"
  exit 77
fi

need() {
  eval "value=\${$1:-}"
  if [ -z "$value" ]; then
    echo "stack-suite: $suite needs $1" >&2
    exit 1
  fi
}

case "$suite" in
  stack-e2e)
    exec bash scripts/stack-e2e.sh ;;
  s3-corpus-eval)
    exec "${PYTHON:-python3}" eval/s3/run.py ;;
  ocr-presets-e2e)
    need GRPARSE_STACK_IMAGE
    need GRPARSE_STACK_MODELS_DIR
    exec bash scripts/test/ocr-presets-e2e-test.sh "$GRPARSE_STACK_IMAGE" "$GRPARSE_STACK_MODELS_DIR" ;;
  embedding-rpc)
    if [ -n "${EMBEDDING_RPC_FIXTURE:-}" ]; then
      exec bash scripts/test/embedding-rpc-test.sh "$EMBEDDING_RPC_FIXTURE"
    fi
    exec bash scripts/test/embedding-rpc-test.sh ;;
  compose-preflight)
    exec bash scripts/test/compose-preflight-test.sh ;;
  observability-compose)
    exec bash scripts/test/observability-compose-test.sh ;;
  *)
    echo "stack-suite: unknown suite '$suite'" >&2
    exit 64 ;;
esac
