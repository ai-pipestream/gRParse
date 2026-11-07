#!/bin/sh
# Decides whether a pull request needs the cuda and openvino image legs
# (#241). Reads the changed paths, one per line, on stdin and prints
# "ep=true" or "ep=false" for $GITHUB_OUTPUT.
#
# The three image jobs run the same ctest suite and only cpu-image has the
# models, so what the cuda and openvino legs add on a PR is compiling the
# execution-provider code and boot-proving their runtime closures. They run
# when one of those inputs, or the CI plumbing they share, changes; main
# pushes (publish.yml), the `ci:full` label and manual runs build all three
# regardless (the workflow's own condition).
#
#   git diff --name-only "$BASE"...HEAD | sh scripts/ci-changes.sh
set -eu

ep=false
while IFS= read -r path; do
  case "$path" in
    Dockerfile|Dockerfile.openvino|.dockerignore|CMakeLists.txt) ep=true ;;
    cmake/*|patches/*|compat/*) ep=true ;;
    src/ort_session_ep.cpp|src/embeddings/*|src/embedding.cpp|src/embed_text_client.cpp) ep=true ;;
    include/grparse/embedding.h) ep=true ;;
    scripts/stage-runtime-closure.sh|scripts/smoke-test.sh) ep=true ;;
    scripts/find-seed.sh|scripts/ccache-mount.sh|scripts/build-jobs.sh|scripts/stamp-sources.sh) ep=true ;;
    scripts/fetch-embedding-model.sh|scripts/test/fetch-embedding-model-test.sh) ep=true ;;
    scripts/ci-changes.sh|.github/workflows/ci.yml) ep=true ;;
  esac
done
echo "ep=$ep"
