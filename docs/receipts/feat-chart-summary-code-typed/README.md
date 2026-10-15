# Receipts: feat/chart-summary-code-typed

Code commit under test: 1a3e87776a9db20483a7d5d4710eea9e1b4751d2 (77eb5eb merged with feat/heading-options-typed-errors at c325153).

| File | Command | Result |
|---|---|---|
| ctest-full.txt | fresh configure and build (GRPARSE_WERROR=ON, OpenCV 5.0.0), then `LD_LIBRARY_PATH=$HOME/.local/opt/opencv5/lib ctest --test-dir /work/tmp/grparse-a-build --output-on-failure -L grparse` | 95/95 passed, exit 0. Skipped tests: the 6 model-gated tests, plus chart-outputs-e2e-test, which needs the backends below. |
| chart-outputs-e2e.txt | `scripts/test/chart-outputs-e2e-test.sh /work/tmp/grparse-a-build grparse-e2e/grpc-enrich:93eedfd` | e2e-exit 0. A real grpc-enrich container, built from grpc-enrich origin/main 93eedfd, runs against the emulated OpenAI-compatible VLM in scripts/test/chart-outputs-e2e. A typed `ChartExtractionPresetRegistry` preset resolves from `ConvertDocumentOptions.chart_extraction_preset`. grpc-enrich makes three VLM calls (csv, summary, code). The table lands in meta.tabular_chart, the summary in meta.description, and the code in meta.code as Python with the code fence removed. |
| grpc-enrich-image-build.txt.gz | `git archive 93eedfd` of grpc-enrich, then `docker build -t grparse-e2e/grpc-enrich:93eedfd .` | enrich-build-exit 0. Nothing in grpc-enrich was changed. |

CI status for this branch is recorded in ci-status-*.txt once the runs finish.
