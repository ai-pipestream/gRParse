# Receipts: feat/heading-options-typed-errors (PR #21)

Code commit under test: e8d1c933562c824521d38ba7c19d523cbd289537

| File | Command | Result |
|---|---|---|
| ctest-full.txt | `LD_LIBRARY_PATH=$HOME/.local/opt/opencv5/lib ctest --test-dir /work/tmp/grparse-a-build --output-on-failure -L grparse` (OpenCV 5.0.0, GRPARSE_WERROR=ON) | 94/94 passed (6 model-gated tests skip locally), exit 0 |
| docker-cuda-build.txt.gz | cuda-image CI steps reproduced locally: docker build of the cuda target, then `scripts/smoke-test.sh` | 94/94 passed, docker-build-exit 0, smoke OK |
| docker-cpu-build.txt.gz | cpu-image CI steps reproduced locally from a detached worktree at e8d1c93: models image, model verify, cpu build with GRPARSE_TEST_REQUIRE_MODELS=1, `scripts/smoke-test.sh grparse-prb-cpu` | 100% tests passed, 0 failed out of 94; docker-build-exit 0; smoke-exit 0; outer-exit 0 |
| ci-status-e8d1c93.txt | Forgejo commit status API | cuda-image, openvino-image, fuzz-smoke and demo-shell pass. cpu-image failed after 24s on a remote runner before the build started. This is a blocker; the full cpu job passes locally (see docker-cpu-build.txt.gz) |

## Run 413 and the merges of feat/parity-1.37.0

| File | Commit | Result |
|---|---|---|
| ci-status-f6c2691-run413.txt | f6c2691, Forgejo run 413 (final) | cuda-image 11m10s, openvino-image, fuzz-smoke and demo-shell pass. cpu-image failed after 6s, before the main CI fix. |
| ctest-112a867.txt | 112a867 (merge of #20 after main f0f190b) | 94/94 passed, exit 0 |
| ci-status-112a867.txt | 112a867, Forgejo run 415 | the image jobs were cancelled when c325153 was pushed. fuzz-smoke and demo-shell pass. |
| ctest-c325153.txt | c325153 (merge of #20 with the dhi.io login step) | 94/94 passed, exit 0 |
| ci-status-c325153.txt | c325153, Forgejo run 419 | cuda-image and cpu-image failed after 4s, the same failure as #20 run 418. fuzz-smoke and demo-shell pass. |

Command for the ctest files: `LD_LIBRARY_PATH=$HOME/.local/opt/opencv5/lib ctest --test-dir <build> --output-on-failure -L grparse` (fresh configure and build, GRPARSE_WERROR=ON, OpenCV 5.0.0).
