# Receipts: feat/parity-core-2.103-slim-2.140

Box: krick. Build dir `/work/tmp/grparse-2140-build`, configured Release, Ninja,
`-DGRPARSE_WERROR=ON -DBUILD_TESTING=ON`, OpenCV 5 from `~/.local/opt/opencv5`,
libcurl dev headers from `~/.local/opt/curl-dev` (libcurl4-openssl-dev, unpacked).

| Receipt | What it shows |
|---|---|
| `build-3370895.txt`, `ctest-3370895.txt` | Branch head: build exit 0, `ctest -L grparse` 100% of 94 passed (6 model-gated tests skipped, as on the base branch). |
| `build-ddf42dc.txt`, `ctest-ddf42dc.txt` | First full run: confidence-test failed on a wrong expected grade in the new pages test (mean 0.883 is GOOD, not EXCELLENT). Fixed in 3370895; kept as the record. |
| `chunk-overflow-before-fix.txt` | The new non-additive join test fails against the old split: `chunk over budget: 8 > 6`. |
| `chunk-overflow-after-fix.txt` | Same tests pass with the convergence loop. |
| `header_rows_reference.py`, `header-rows-reference.txt` | docling-core 2.103.0's own `export_to_markdown` for the five header-row cases; the C++ test expects these strings byte for byte. Run with the docling-core fork venv at d572007. |

Commands:

```
cmake --build /work/tmp/grparse-2140-build --parallel 20
LD_LIBRARY_PATH=$HOME/.local/opt/opencv5/lib ctest --test-dir /work/tmp/grparse-2140-build --output-on-failure -L grparse
/work/docling-research/docling-core/.venv/bin/python header_rows_reference.py
```
