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
