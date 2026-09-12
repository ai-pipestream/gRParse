# Local chunk embeddings: acceptance work

## Verification ledger

The final-source checkpoints below include the verified-buffer loader
and optional benchmark mode. Older CPU and Intel results farther below
are retained as historical checkpoints. Hosted CI remains outstanding.

2026-09-12, final-source CPU build
`sha256:5188e1995e2c3926669117af9c72615d9934ea293a02143a451cb26116a41f21`:

- Build exited 0 on krick-1; 79 CTest entries, no failures, six
  model-dependent skips. Log: `~/builds/local-embeddings-cpu-final.log`.
- Packaged smoke, required model/reference checks and the full CPU RPC
  suite all exited 0. RPC artifacts: `krick-1:~/builds/embedding-rpc.2lt8beJ2/`.

2026-09-12, final-source Intel image
`sha256:3d18f3e7c358b7893bebc2497fb60604aac676456f87d0c9890c85837008d2a4`:

- Build exited 0; 79 CTest entries, no failures, six model-dependent skips.
  Log: `krick-1:~/builds/local-embeddings-openvino-final.log`.
- Packaged smoke, required native GPU model check and independent reference
  exited 0. Official OpenVINO IR CPU FP32 comparison passed, with maximum
  coordinate error `3.06405127e-7`.
- Full Intel GPU RPC suite exited 0, including both chunk methods,
  unchanged documents/chunks, invalid model rejection, deadline/recovery,
  and disabled operation without embedding models. Artifacts:
  `krick-1:~/builds/embedding-rpc.p3Z7l1uI/`.
- Same-image, same-host four-text benchmarks (20 warm repeats) averaged
  2.826 ms/batch on CPU and 0.625 ms/batch on Intel GPU. Initialization
  took 305 ms and 1726 ms; process peak RSS was 267704 KiB and 956268 KiB,
  respectively. These measure host process memory, not GPU VRAM.

The serialized CPU/Intel verifier exited 0. Per-command stdout, stderr,
image identities and exit files are retained at
`krick-1:~/builds/verify-embeddings-final.1qJOjgK4/`; its final log is
`krick-1:~/builds/verify-embeddings-final.log`.

2026-09-12, NVIDIA image
`sha256:4d52ff247e1ec20a8554785a96d507e5fb1eb3222312525564e63af9c5788cde`:

- Final-source build exited 0 on krick-1; 79 CTest entries, no failures,
  six model-dependent skips. Source checksums matched the local worktree.
  Log: `krick-1:~/builds/local-embeddings-cuda-final.log`.
- Transferred image digest matched. Runtime smoke exited 0 on the RTX
  4080 SUPER host (driver 595.84).
- Required native TensorRT model check exited 0 on the physical GPU,
  including CPU agreement, padding, normalization, retrieval and limits.
  Log: `/tmp/grparse-final-nvidia-check.log`.
- Independent native reference exited 0; maximum coordinate error
  `1.56462193e-7`. Log: `/tmp/grparse-final-cuda-reference.log`.
- Full TensorRT-backed RPC suite exited 0, including both chunk methods,
  unchanged documents/chunks, invalid model rejection, deadline/recovery,
  and a disabled server without embedding models. Artifacts:
  `/home/krickert/builds/embedding-rpc.VVG0MgUU/` on the RTX host.
- A four-text, 20-repeat warm embedding benchmark on that same host
  averaged 3.544 ms/batch on CPU and 0.552 ms/batch on TensorRT. Model
  initialization took 328 ms and 3123 ms respectively; process peak RSS
  was 457572 KiB and 2447476 KiB. These are small-batch embedding timings
  and host process memory, not end-to-end parser throughput or GPU VRAM.
  Results: `/tmp/grparse-final-benchmark-{cpu,tensorrt}.json`.

2026-09-12, CPU image
`sha256:d4c8079093a9cc923c727e5ed882c3a3809b0262f1ea3aa20d1bf91ec36b9584`:

- Build exited 0 on krick-1. CTest listed 79 tests with no failures; the
  two model-dependent embedding tiers skipped because the build had no models.
- Runtime smoke passed, including the new shipped executables' library closure.
- Both `grparse-embedding-check` and `grparse-embedding-reference` then exited
  0 with the pinned artifacts mounted and `GRPARSE_EMBEDDING_TEST_REQUIRE=1`.
  The separate ORT/tokenizer reference's maximum coordinate error was
  `1.56462193e-7`; boundary and padding checks passed.
- Packaged `ChunkHybridSource` and `ChunkHierarchicalSource` RPC checks exited
  0: off/on chunks matched after removing only embeddings, full documents
  matched, vectors and provenance passed, invalid model requests failed, and
  a separate embeddings-disabled server without the embedding mount preserved
  output and rejected embedding requests with `FailedPrecondition`. Responses
  and logs: `krick-1:~/builds/embedding-rpc.vwEIRimB/`.
- A warmed enabled server returned `DeadlineExceeded` for a 50 ms RPC
  deadline with no partial JSON (client exit 68); the subsequent enabled
  request returned vectors and unchanged chunks/documents. Logs:
  `krick-1:~/builds/embedding-rpc.9E5kX31F/`. This proves RPC-level deadline
  handling and recovery, not cancellation specifically during inference.
- OpenVINO IR comparison was not compiled into this CPU image; see the
  Intel image's separate results below.

Build logs are on krick-1 at `~/builds/local-embeddings-cpu.log` (incremental
native-test build) and `~/builds/local-embeddings-cpu-first.log` (first build).

2026-09-12, Intel image
`sha256:863820e72c4d7bb5df60373e09e61ee7b9b277bd775bb0142ce430438dd57d96`:

- Build exited 0 with 79 CTests and no failures (two model tiers skipped
  inside the build, then explicitly executed below). Runtime smoke passed.
- Required independent reference exited 0, including official OpenVINO IR
  on CPU. Maximum coordinate error across the IR comparisons was
  `3.06405127e-7`.
- Required native `openvino` GPU model check exited 0 with `/dev/dri`
  exposed and the render group supplied. CPU/GPU agreement, normalization,
  padding, retrieval, token limits and cancellation checks passed.
- Full GPU-backed RPC test exited 0: deadline/recovery, both chunk RPCs,
  off/on content equality, invalid model and disabled-without-model behavior.
  Logs: `krick-1:~/builds/embedding-rpc.mSch1zme/`.

Intel build log: `krick-1:~/builds/local-embeddings-openvino.log`.
Final hosted CI remains outstanding.

The feature adds optional local embeddings to the existing synchronous
hierarchical and hybrid chunk RPCs. Parsing without embedding options must
retain its existing behavior. Async/watch endpoints remain unimplemented.

## Shared model and execution

- Model: sentence-transformers/all-MiniLM-L6-v2 at the revision recorded in
  `models/embeddings/MANIFEST`.
- Inputs: the exact selected chunk text, optionally contextualized with its
  headings. The response records that text and immutable model identity.
- Shared native tokenizer, with embedded padding/truncation disabled; add
  CLS/SEP, reject more than 256 tokens, and pad only within a bounded batch.
- Shared attention-mask mean pooling and L2 normalization, 384 FP32 values.
- CPU: ONNX Runtime without a GPU execution provider.
- Intel: OpenVINO Runtime through its native C API on an explicit GPU,
  directly loading ONNX. The C boundary isolates the SDK's C++ library ABI.
- NVIDIA: TensorRT C++ and CUDA, directly importing the same ONNX model.

This is an in-process pipeline, not a claim that tokenization, pooling or
document parsing remains GPU-resident. GPU kernels currently return hidden
states to shared CPU pooling code. A later optimization can move pooling
onto the device after parity and transfer costs are measured.

## Required gates before completion

1. Compile all variants and pass existing parser tests plus new embedding
   orchestration and RPC tests. Verify disabled behavior, bounded batches,
   exact provenance, cancellation, malformed results and atomic failure.
2. Verify downloaded artifacts against immutable hashes. Exercise offline
   verification and corrupted-file failure.
3. Run real CPU embeddings, including padding invariance, Unicode, empty
   text, repeated text, normalization and oversized-input rejection.
4. Compare CPU results with the published model's reference tokenizer and
   mean-pooling implementation. Check token IDs, vector similarity and a
   small retrieval ordering fixture.
5. Run the same corpus on Intel and NVIDIA hardware. Verify actual device
   selection and compare cosine similarity, retrieval ordering, latency and
   peak memory with CPU. A mocked provider is not evidence for this gate.
6. Run real parse/chunk/embed RPCs and parse-only requests, including
   cancellation, on the packaged images. Check no model is required when
   embedding is disabled.
7. Run required GitHub CI and publication workflows on the final changes.

## Build safety

Heavy builds run on krick-1, one at a time under `~/builds/.build.lock`.
The dedicated `grparse-embeddings` BuildKit container is capped at two CPUs
and 8 GiB, with one concurrent build operation. Dockerfiles also bound C++
and Rust compilation through `GRPARSE_BUILD_JOBS` (default 2). Existing
deployment containers are not stopped or replaced for these checks.

## Native acceptance commands

After building an image, populate the pinned model directory with
`scripts/fetch-embedding-model.sh --backend onnx`. Run the native check:

```bash
docker run --rm --read-only --tmpfs /tmp --cpus 2 --memory 2g \
  -v "$PWD/models/embeddings/all-MiniLM-L6-v2:/embedding-models:ro" \
  -e GRPARSE_EMBEDDING_TEST_MODELS=/embedding-models \
  -e GRPARSE_EMBEDDING_TEST_REQUIRE=1 \
  --entrypoint grparse-embedding-check grparse:local-embeddings-cpu
```

Use the corresponding OpenVINO or CUDA image on the physical accelerator
host, with `GRPARSE_EMBEDDING_TEST_BACKEND=openvino` or `tensorrt` in the
container. Expose Intel devices with `--device /dev/dri` and the render
node's group via `--group-add`, or NVIDIA GPU 0 with `--gpus device=0`.
Allow 8 GiB for GPU engine construction while retaining the two-CPU cap.
The native checks cover real vectors, normalization, repeated text, batch
padding, retrieval ordering, oversized-input rejection and GPU/CPU parity.
Shared-backend parity is not an independent tokenizer/model oracle; that
separate gate and the end-to-end RPC gates above remain required.

`grparse-embedding-reference` supplies the separate integration reference:
the tokenizer's C API adds its own special tokens, an independent ORT
session produces hidden states, and test code applies masked mean pooling
and L2 normalization. In the OpenVINO image it additionally checks the
official XML/bin export on the CPU plugin. Fetch `--backend all` for that
tier and use the same model-directory and REQUIRE environment settings.
This checks our preprocessing and runtime integration independently, but
does not constitute an independent implementation of the tokenizer library
itself. No upstream complete golden-vector fixture was found at the pinned
model revision.

## Packaged RPC checks

`scripts/test/embedding-rpc-test.sh` runs an isolated server and a cached
grpcurl client with no published ports. Its default image/backend is the CPU
acceptance image. On the corresponding physical GPU host, select:

```bash
EMBEDDING_RPC_IMAGE=grparse:local-embeddings-openvino \
EMBEDDING_RPC_BACKEND=openvino scripts/test/embedding-rpc-test.sh

EMBEDDING_RPC_IMAGE=grparse:local-embeddings-cuda \
EMBEDDING_RPC_BACKEND=tensorrt scripts/test/embedding-rpc-test.sh
```

Override `EMBEDDING_RPC_OCR_MODELS` and `EMBEDDING_RPC_EMBEDDING_MODELS`
when the host's files differ from the krick-1 defaults in the script. A
fixture path may be supplied as its positional argument. This test keeps
OCR on CPU to isolate the embedding backend; native OCR acceleration has
its own parser acceptance checks. Startup waits are bounded by
`EMBEDDING_RPC_STARTUP_SECONDS` (300 by default, at most 900). Logs and
responses remain under `~/builds/embedding-rpc.*`; only test-owned containers
are removed.

## Timing and process memory

Set `GRPARSE_EMBED_BENCHMARK_REPEATS=20` when invoking
`grparse-embed-text` to collect model-load time and warm batch latency
after one warmup. The optional JSON `benchmark` object also reports Linux
process-lifetime peak RSS in KiB. This is host process memory, not GPU VRAM.
Use the same texts, batch size and CPU limits for each comparison; keep
CPU/GPU comparisons on the same host and report the host when presenting
results. The benchmark does not establish general parser throughput.
