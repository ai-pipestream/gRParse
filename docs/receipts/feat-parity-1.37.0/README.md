# Receipts: PR #20 (Docling 1.37.0 parity, workstream A)

- `ctest-full.txt`: build and the full `ctest -L grparse` run on krick at commit
  96260c0984eff92393f1c28a4689b7afe3a8ee13 (the code commit of this PR; this
  receipts commit adds files under docs/ only). The exact commands are at the
  top of the file. Result: 94 tests, 88 passed, 6 skipped, 0 failed. The six
  skips are the model-golden tests (chunk-hf-model, figure-classifier,
  layout-engine, table-structure-engine, embedding-reference,
  embedding-model): krick has no pinned model files, so they skip. The CI
  cpu-image job runs them with the models provided.
- `opencv5-env.txt` and `opencv5-build.log.gz`: the OpenCV 5.0.0 build that
  unblocks local ctest on krick (same version, checksum and module list as
  the Dockerfile), installed to a user prefix.
- New tests in this PR: convert-options-test `verify_parity_1_37_options`,
  heading-hierarchy-test `verify_numbering_schemes`, chart-derender-test
  `verify_code_formula_options_reach_enrich_typed` (a real in-process gRPC
  fake enrich server) and `verify_repetition_truncation_folds_typed`,
  input-format-test Numbers mappings, and collector-coordinator-test Numbers
  routing.
- `docker-cuda-build.txt.gz`: local run of the CI cuda-image step
  (`docker build --progress=plain -t grparse-pr20-cuda .`) at commit
  e7fed6acd9892ac369368af8aec2818adac62739. It covers the full image build
  and the in-build `ctest -L grparse`: 100% passed, 0 failed out of 94, and
  `docker-build-exit: 0`.
- `smoke-cuda.txt`: `./scripts/smoke-test.sh grparse-pr20-cuda` on that
  image, `smoke-exit: 0`.
- `ci-status-e7fed6a.txt`: Forgejo CI on e7fed6a. demo-shell, fuzz-smoke,
  openvino-image and cpu-image passed. cuda-image failed after 34s on a
  remote runner, and Forgejo does not expose that job's log through the API.
  main (031cc23) shows the same 34-51s cuda-image failure. The local run of
  the same step above is green. This is a blocker on the runner, not a
  passing result.

## After merging main (f0f190b) and adding the dhi.io login

Command for both ctest files: `LD_LIBRARY_PATH=$HOME/.local/opt/opencv5/lib ctest --test-dir <build> --output-on-failure -L grparse`, after a fresh configure and build with GRPARSE_WERROR=ON and OpenCV 5.0.0.

| File | Commit | Result |
|---|---|---|
| ctest-merge-main-6299297.txt | 6299297 (merge of main f0f190b) | 94/94 passed, exit 0 |
| ci-status-6299297.txt | 6299297, Forgejo run 414 | all five jobs green: cuda-image 24m51s, cpu-image 12m04s, openvino-image, fuzz-smoke, demo-shell |
| ctest-1d56cdf.txt | 1d56cdf (dhi.io login step, full Dockerfile.models build) | 94/94 passed, exit 0 |
| ci-status-1d56cdf.txt | 1d56cdf, Forgejo run 418 | cuda-image failed after 5s and cpu-image after 19s. These are the two jobs that gained the dhi.io login step. openvino-image and demo-shell pass. Blocker: the job logs are not available through the API. |
