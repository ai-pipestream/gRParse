# Receipts: S3 and HTTP sources

All receipts below were run on feature commit 4859e0a8adc5f6b13909579671a864bbb26664f4 (each file's first line names it).

| File | Command | Result |
|---|---|---|
| `ctest-4859e0a.txt` | `/work/tmp/receipts-merge/run.sh` (configure with `-DGRPARSE_WERROR=ON`, build, `ctest --output-on-failure -L grparse`) | 99/99 passed (the e2e and model tests that need their backends skip with 77 and run separately) |
| `e2e-4859e0a.txt` | `scripts/test/remote-sources-e2e-test.sh /work/tmp/grparse-a-build` | exit 0: MinIO http and self-signed https (verify_ssl unset and true refused, false accepted), local http origin (plain, redirect, Content-Disposition, request header, 401, 404, 500, redirect loop, file:// redirect, body cap) and self-signed https origin refused |
| `storage-e2e-4859e0a.txt` | `scripts/test/storage-connectors-e2e-test.sh /work/tmp/grparse-a-build` | exit 0: the existing storage connectors still pass after the shared http_call and S3 credential changes |

The "Exception occurred during processing of request" lines in the https origin log are the origin's side of the TLS handshakes the client refused, which is the expected outcome.

CI results are recorded as PR comments, not commits.
