# Fuzzing the ingest door

The raster ingest door is the only in-process code path that touches
attacker-controlled bytes before any model runs. PDFs never parse in this
process: they go to the configured PdfBackendService (`GRPARSE_PDF_BACKEND`),
whose engine is fuzzed in its own repository.

| Target | Door | Surface |
| --- | --- | --- |
| `raster-source-fuzzer` | `RasterPageSource` | in-memory `cv::imdecode` |

The contract under test: malformed bytes fail with `InvalidDocument`, never a
crash, hang, or sanitizer finding.

This is a standalone CMake project on purpose - the door needs only OpenCV
and `../include`, so a fuzz build skips the gRPC/ONNX Runtime fetch and
configures in seconds. libFuzzer requires Clang.

```bash
sudo apt-get install -y clang libclang-rt-dev ninja-build cmake pkg-config libopencv-dev
cmake -S fuzz -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz

mkdir -p corpus-raster
LSAN_OPTIONS=suppressions=tests/lsan.supp \
  ./build-fuzz/raster-source-fuzzer corpus-raster tests/data -max_len=1048576
```

The first corpus directory is writable (new coverage lands there); the second
is the read-only seed set, the committed PNG fixtures under `tests/data`.

CI runs the fuzzer for a short smoke window on every push and PR (the
`fuzz-smoke` job in `.github/workflows/ci.yml`); longer campaigns are run
manually with the commands above, without `-max_total_time`.
