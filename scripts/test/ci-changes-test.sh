#!/bin/sh
# Pins scripts/ci-changes.sh (#241): which changed paths pull the cuda and
# openvino image legs into a pull request run. Run from the repo root.
set -eu

script=scripts/ci-changes.sh
fail=0

expect() {
  want=$1
  shift
  got=$(printf '%s\n' "$@" | sh "$script")
  if [ "$got" != "ep=$want" ]; then
    echo "FAIL: [$*] gave '$got', want 'ep=$want'" >&2
    fail=1
  fi
}

# Plain first-party sources, tests, docs and eval run on cpu-image alone.
expect false src/source_parse.cpp tests/convert_options_test.cpp docs/COLLECTORS.md
expect false src/render/renderer_base.cpp include/grparse/ocr_engine.h
expect false eval/scorecard/run.py examples/web-demo/server.js README.md
expect false Dockerfile.cpu e2e/specs/proxy.spec.ts compose.stack.yaml
expect false .github/workflows/publish.yml
# No changed paths at all (an empty diff) is not a reason for the heavy legs.
printf '' | sh "$script" | grep -qx 'ep=false' || { echo "FAIL: empty diff" >&2; fail=1; }

# Execution-provider code and the image plumbing pull both legs in.
expect true src/ort_session_ep.cpp
expect true src/source_parse.cpp src/embeddings/engine.cpp
expect true src/embeddings/nvidia_backend.cpp
expect true src/embedding.cpp
expect true src/embed_text_client.cpp
expect true include/grparse/embedding.h
expect true compat/grparse_session_ep.h
expect true Dockerfile
expect true Dockerfile.openvino
expect true CMakeLists.txt
expect true cmake/Embeddings.cmake
expect true patches/grpc/fix.patch
expect true scripts/stage-runtime-closure.sh
expect true scripts/smoke-test.sh
expect true scripts/find-seed.sh
expect true .github/workflows/ci.yml
expect true scripts/ci-changes.sh

# Look-alike paths stay out.
expect false src/embeddings_notes.md docs/Dockerfile.md tests/cmake_test.cpp

[ "$fail" -eq 0 ] && echo "ci-changes-test: ok"
exit "$fail"
