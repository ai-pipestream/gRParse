# Fuzzing the ingest doors

The two ingest doors are the only code paths that touch attacker-controlled
bytes before any model runs:

| Target | Door | Surface |
| --- | --- | --- |
| `pdf-source-fuzzer` | `PdfPageSource` | Poppler open + digital-text extraction |
| `raster-source-fuzzer` | `RasterPageSource` | in-memory `cv::imdecode` |

The contract under test: malformed bytes fail with `InvalidDocument`, never a
crash, hang, or sanitizer finding.

This is a standalone CMake project on purpose - the doors need only OpenCV,
Poppler, and `../include`, so a fuzz build skips the gRPC/ONNX Runtime fetch
and configures in seconds. libFuzzer requires Clang.

```bash
sudo apt-get install -y clang libclang-rt-dev ninja-build cmake pkg-config \
  libopencv-dev libpoppler-cpp-dev
cmake -S fuzz -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz

mkdir -p corpus-pdf corpus-raster
LSAN_OPTIONS=suppressions=tests/lsan.supp \
  ./build-fuzz/pdf-source-fuzzer corpus-pdf fuzz/seeds/pdf -max_len=1048576
LSAN_OPTIONS=suppressions=tests/lsan.supp \
  ./build-fuzz/raster-source-fuzzer corpus-raster tests/data -max_len=1048576
```

The first corpus directory is writable (new coverage lands there); the second
is the read-only seed set. Raster seeds reuse the committed PNG fixtures under
`tests/data`; PDF seeds live in `fuzz/seeds/pdf`. The fontconfig/expat leak
suppressions in `tests/lsan.supp` cover Poppler's one-time global font
configuration allocation.

CI runs both fuzzers for a short smoke window on every push and PR (the
`fuzz-smoke` job in `.github/workflows/ci.yml`); longer campaigns are run
manually with the commands above, without `-max_total_time`.
