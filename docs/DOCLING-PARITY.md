# Docling parity notes

gRParse mirrors the Docling and docling-serve option surface in typed
protobuf. Where gRParse does something different, or does not do something
upstream does, the reason is written down here so the gap is a decision and
not an accident.

## docling-serve 1.37.0

| Upstream change | gRParse | Why |
|---|---|---|
| `InputFormat.IWORK_NUMBERS` | `INPUT_FORMAT_IWORK_NUMBERS = 36`; `.numbers`, `application/vnd.apple.numbers` and `application/x-iwork-numbers-sffnumbers` route to the libreoffice collector | LibreOffice's Calc import reads Numbers packages, so each sheet comes back as a typed table through the same path as ODS and XLSX. calamine does not read Numbers, so there is no calamine fan-out for it. |
| `CodeFormulaVlmOptions.expansion_factor`, `stop_on_repetition` | Typed optional fields 7 and 8, validated (finite, >= 0) and sent to grpc-enrich as `EnrichOptions.code_formula_expansion_factor` / `code_formula_stop_on_repetition` | The code and formula crops and the model calls happen in grpc-enrich. A cancelled, repeating generation comes back with `truncated_on_repetition`, and gRParse records it on the item as a `GenerationSource` with `truncation = GENERATION_TRUNCATION_REPETITION`. |
| `ChartExtractionOutputFormat.GRANITE_VISION_CHART2CSV` | `CHART_EXTRACTION_OUTPUT_FORMAT_GRANITE_VISION_CHART2CSV = 2`, also accepted as the raw name `granite_vision_chart2csv` | Same validator as Docling: CSV output only. |
| Default code/formula preset (#721) | `code_formula_preset` resolves to the typed `CodeFormulaPresetId`; unset, empty and `default` are `CODEFORMULAV2`, the Docling default; an unregistered name is `INVALID_ARGUMENT` | The preset travels to grpc-enrich as its typed `CodeFormulaPreset`, never as a raw model name. |
| Heading `numbering_schemes` | Typed `repeated HeadingNumberingScheme numbering_schemes = 10`; Docling's string list is accepted as `numbering_schemes_raw` (field 5) and mapped by name | Unknown or unspecified values are rejected by value. A non-empty list replaces the built-in vocabulary: the listed order is the precedence. |
| Ray `ray_task_timeout` / `ray_job_timeout` (#725) | Not implemented | gRParse has no Ray orchestrator. Every request runs in process on the server's own executor, bounded by `document_timeout` and the call deadline, so there is no Ray task or job to time out. |

## Heading options and typed rejections

| Option | gRParse | Notes |
|---|---|---|
| `use_bookmarks` (default true) | Implemented | A heading takes the depth of the best-matching `Document.outline` entry. The entry must be on the same page when both name one, and its title similarity must reach `bookmark_match_threshold`. A document without an outline is unaffected, which is the same as Docling on a PDF without bookmarks. |
| `bookmark_match_threshold` (default 0.8) | Implemented, validated in [0, 1] | Similarity is 1 minus the edit distance over the longer normalized title. Normalizing lower-cases the title and collapses punctuation and white space. |
| `use_font_style` (default true) | Implemented | In the style pass, among headings of one size, bold ranks above regular, upright above italic, and all-caps above mixed case. Weight and slant come from the item formatting or the majority of its runs. A heading whose runs carry no font information ranks by size alone. |

Every INVALID_ARGUMENT that turns an option down carries a typed `OptionRejection` (option path, `OptionRejectionReason`, the raw rejected value). It is packed in `google.rpc.Status.details`, so clients read it from the standard `grpc-status-details-bin` trailer.

`S3Source.verify_ssl` is now `optional bool` (same tag 6). An unset value verifies; only an explicit false skips verification. The old implicit bool read an unset field as false.
