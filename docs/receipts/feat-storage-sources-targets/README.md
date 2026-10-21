# Receipts: storage sources and targets

Feature commit chain: 19848d9 (connectors), 48e2e5b (put and
presigned_url stay UNIMPLEMENTED). All receipts below ran on 48e2e5b.

| Receipt | Command | Result |
|---|---|---|
| `ctest-48e2e5b.txt` | `/work/tmp/receipts-merge/run.sh` (configure, build with `GRPARSE_WERROR=ON`, `ctest -L grparse --output-on-failure`) | configure 0, build 0, 97/97 passed (skips are the model tests and the two e2e tests, which skip with 77 without their backends) |
| `e2e-48e2e5b.txt` | `scripts/test/storage-connectors-e2e-test.sh /work/tmp/grparse-a-build` | exit 0 |

The e2e run starts fake-gcs-server 1.52.2, Azurite 3.34.0 and MinIO, plus
the mock Google token endpoint and Drive v3 API
(`scripts/test/storage-connectors-e2e/mock_google.py`), and seeds Azurite
with an independent Shared Key signer. It proves:

- sources: Azure Blob, Cloud Storage (anonymous emulator and a service
  account whose RS256 assertion the mock verifies with the public key),
  Drive (refresh-token grant) and a registered plugin each resolve to the
  seeded file, byte for byte;
- refusals against the real backends: an empty prefix, a prefix holding
  two blobs, a Drive folder and a native Google document are
  INVALID_ARGUMENT naming the field; a refused refresh token is UNAVAILABLE;
- targets: Azure Blob, Cloud Storage, Drive, S3 (MinIO) and the plugin
  each receive the bundle through `targets::deliver`; every object is read
  back from its store and compared with the bundle; Drive reports the file
  ids as `store_id`;
- parity: all connectors write the same bundle member paths as the S3
  target, and `mc ls` on MinIO shows exactly the keys the S3 target reported.

A first ctest run on 19848d9 failed `streaming-service-test` (the early
target policy check refused the unserved `put` target as NOT_ALLOWED
instead of UNIMPLEMENTED). 48e2e5b fixes it; that run was not kept as a
receipt because it was superseded on the same branch before push.

CI status is recorded in `ci-status-*.txt` once the runs finish.
