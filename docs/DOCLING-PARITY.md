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

## Chart extraction: chart2summary and chart2code

| Option | gRParse | Notes |
|---|---|---|
| `ChartExtractionVlmEngineOptions.chart2summary` | Implemented | Sent to grpc-enrich as `ChartExtractionOptions.summary`. The reply lands in the picture's `meta.description` (created_by the model) with a description annotation and a `GenerationSource`, where Docling stores it. |
| `ChartExtractionVlmEngineOptions.chart2code` | Implemented | Sent as `ChartExtractionOptions.code`. grpc-enrich takes the Python out of the reply's code fence, and gRParse stores it in the picture's `meta.code` with `CODE_LANGUAGE_LABEL_PYTHON`. |
| `custom_chart_extraction_presets` | Typed `ChartExtractionPresetRegistry` of `ChartExtractionPresetSpec` | `GRPARSE_CUSTOM_CHART_EXTRACTION_PRESETS` is that message in protobuf text format; it used to be a free-form JSON object read through `google.protobuf.Struct`. The protobuf text parser rejects unknown fields, wrong types (a quoted `"yes"` for a bool, for example) and unknown enum values. `engine_type` is a `VlmEngineType` name such as `VLM_ENGINE_TYPE_API_OLLAMA`, so Docling's lowercase engine names are rejected in this variable. This changes the variable's format, and existing deployments must rewrite it. compose.stack.yaml is updated. |
| `allowed_chart_extraction_engines` | Mapped to `VlmEngineType` at startup | The variable still takes Docling's engine names, because that is the docling-serve setting. Each name maps to the enum, and an unknown name stops the process. The policy compares enums, not strings. List variables in JSON array form are read as the typed `ServerSettingNameList`. |

Each enabled output is its own VLM call per chart in grpc-enrich, so a failed summary never costs the table. grpc-enrich has implemented the three outputs since 05820b7, so this needed no change there. `scripts/test/chart-outputs-e2e-test.sh` runs a real grpc-enrich container against an emulated OpenAI-compatible VLM, and checks that the table, summary and code arrive in the Document.
