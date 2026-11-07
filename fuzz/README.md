# Fuzzing the in-process doors

Several in-process code paths read attacker-controlled request bytes before
any model or collector runs, and each has a libFuzzer target here. PDFs
never parse in this process: they go to the configured PdfBackendService
(`GRPARSE_PDF_BACKEND`), whose engine is fuzzed in its own repository.

| Target | Door | Surface |
| --- | --- | --- |
| `raster-source-fuzzer` | `RasterPageSource` | the container-magic gate and in-memory `cv::imdecode` |
| `cfb-fuzzer` | `encrypted_office_document`, `sniff_mimetype` | the [MS-CFB] compound-file walker in `src/content_sniff.cpp`: FAT through the DIFAT chain, directory chain, mini FAT and mini stream, stream heads (FIB, BIFF FILEPASS, Current User) |
| `content-sniff-fuzzer` | `resolve_mimetype`, `input_format_for` | every upload's routing: declared type, zip central-directory and mimetype-entry peeks, binary signatures, the text, markup, mail, Markdown and CSV ladders, the extension map |
| `confluence-storage-fuzzer` | `COLLECTOR_CONFLUENCE` | the storage-format XHTML pull parser (`src/confluence/storage_parser.cpp`) and its fold into a Document |
| `base64-differential-fuzzer` | `decode_base64`, `encode_base64` | simdutf's vector kernels against the scalar contract oracle |

The contract under test: malformed bytes fail the way the door documents
(`InvalidDocument`, an empty sniff, `std::invalid_argument`), never with a
crash, hang, or sanitizer finding. On top of that, each target checks what
its door promises:

- `cfb-fuzzer` and `content-sniff-fuzzer` hold the door to a heap budget of
  16 bytes per input byte plus 4 MiB, measured with the sanitizer allocator
  hooks (`heap_budget.h`), so a memory amplification like #225 (a DIFAT
  chain inflating the FAT to 127 times the file) is a crash, not a slow
  run that only `-rss_limit_mb` would notice.
- `confluence-storage-fuzzer` drives the parser and the fold without the
  entry point's catch-all, so an exception is a finding, and requires the
  Document to pass `docling_integrity_errors` and the docling-core
  structural rules (`docling_structure_findings`). One rule is left out,
  LIST_GROUP_NON_LIST_ITEM_CHILD: the fold hangs a nested list off the
  enclosing list group by design, where docling-core wants it under the
  list item, so every nested list trips it (a known finding, kept in
  `regressions/confluence-storage-fuzzer/nested-list-under-group.storage.xhtml`).
- `base64-differential-fuzzer` requires the two decoders to accept and
  reject the same inputs with the same bytes, the two encoders to agree,
  and an encoding to round-trip with whitespace spliced in and to be
  rejected with its padding cut.

`document_passwords.cpp` reads its candidates from gRPC call metadata, not
from the document, under fixed count and length bounds; the encrypted
office header it pairs with is `encrypted_office_document`, which
`cfb-fuzzer` and `content-sniff-fuzzer` drive.

This is a standalone CMake project on purpose: the doors need OpenCV, the
system protobuf (the Document and InputFormat messages, generated from the
repository's protos), simdutf (fetched at the tag the top-level build pins)
and the sources under test, so a fuzz build skips the gRPC/ONNX Runtime
fetch and builds in about a minute. libFuzzer requires Clang.

```bash
sudo apt-get install -y clang libclang-rt-dev ninja-build cmake pkg-config git \
  libopencv-dev libprotobuf-dev protobuf-compiler
cmake -S fuzz -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz

export LSAN_OPTIONS=suppressions=tests/lsan.supp
mkdir -p corpus-raster corpus-cfb corpus-content-sniff corpus-confluence-storage corpus-base64
./build-fuzz/raster-source-fuzzer corpus-raster tests/data -max_len=1048576
./build-fuzz/cfb-fuzzer corpus-cfb fuzz/corpus/cfb fuzz/regressions/cfb-fuzzer -max_len=1048576
./build-fuzz/content-sniff-fuzzer corpus-content-sniff tests/golden/corpus \
  fuzz/corpus/cfb fuzz/regressions/cfb-fuzzer -max_len=1048576
./build-fuzz/confluence-storage-fuzzer corpus-confluence-storage \
  fuzz/corpus/confluence-storage fuzz/regressions/confluence-storage-fuzzer -max_len=65536
./build-fuzz/base64-differential-fuzzer corpus-base64 \
  fuzz/corpus/base64 fuzz/regressions/base64-differential-fuzzer -max_len=16384
```

The first corpus directory is writable (new coverage lands there); the
rest are read-only seed sets: the PNG fixtures under `tests/data`, the
scorecard corpus under `tests/golden/corpus`, and the small hand-made seeds
under `fuzz/corpus/<door>/` (the compound files were written with
`tests/support/compound_file.h`).

`fuzz/regressions/<target>/` keeps every input a fuzzer has found, and each
is replayed on every run as a seed. The ones behind a fix are also unit
tests: `content-sniff-test` replays the compound-file regressions under the
same heap budget (`verify_fuzz_regressions_stay_in_budget`; #225's input is
`issue-225-difat-loop.cfb`), `base64-test` carries the whitespace-only
inputs the vector decoder used to accept, and `confluence-storage-test`
carries the empty lists that used to leave an empty group
(`verify_empty_lists_are_dropped`).

CI runs every target for a 90 s smoke window on every push and PR (the
`fuzz-smoke` job in `.github/workflows/ci.yml`), and for 20 minutes each in
the nightly heavy tier (`fuzz-long` in `.github/workflows/nightly.yml`),
which keeps the grown corpora as an artifact. Longer campaigns run with the
commands above, without `-max_total_time`.
