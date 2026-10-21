# Receipts: typed custom OCR preset registry

Commits: a584bc5 (registry), b879219, d16bab4, 3f6576c (e2e script
fixes). All receipts below ran on 3f6576c.

| Receipt | Command | Result |
|---|---|---|
| `ctest-3f6576c.txt` | `/work/tmp/receipts-merge/run.sh` (configure, build with `GRPARSE_WERROR=ON`, `ctest -L grparse --output-on-failure`) | configure 0, build 0, 98/98 passed (skips are the model tests and the e2e tests that need backends) |
| `e2e-3f6576c.txt` | `scripts/test/ocr-presets-e2e-test.sh grparse-e2e/grparse:3f6576c <models>` against the server image built from this commit's tree (`git archive HEAD \| docker build`), on the GPU | exit 0 |

The e2e run proves, through ConvertSource over gRPC:

- a registry with an unreadable language (`korean`) stops startup with
  the typed error;
- with `GRPARSE_ALLOWED_OCR_PRESETS=rapidocr` and a custom
  `chinese_doc` preset, `ocr_preset: chinese_doc` converts
  tests/data/report_page.png with CONVERSION_STATUS_SUCCESS, every text
  item sourced from rapidocr (docling-serve #723: custom presets stay
  selectable);
- `auto` (removed by the allow list) and `easyocr` (never served) are
  INVALID_ARGUMENT with OptionRejection{ocr_preset, NOT_ALLOWED, the
  name}, worded as docling-serve does;
- a custom preset of kind EASYOCR is OptionRejection NOT_IMPLEMENTED.

Earlier runs of the e2e script on a584bc5 and b879219 failed in the
script's own JSON checks (oneof wrapper, nested collector sources, proto
field names); the three follow-up commits fix the script, and no failing
run is cited as a pass.

CI status is recorded in `ci-status-*.txt` once the runs finish.
