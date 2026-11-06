# Documented deviations: Docling serve golden vs gRParse

Serve gRPC (`ai.docling.core.v1.DoclingDocument`) is the reference for the
data gRParse should reproduce. gRParse's wire type is
`ai.pipestream.document.v1.Document`, field-number compatible with the
upstream dialect plus additive extensions.

| Topic | Serve / Docling | gRParse | Expectation in tests |
|---|---|---|---|
| `schema_name` | `DoclingDocument` | `docling_document_v2` | Normalize before compare |
| `PageQuality.text_layer_replaced` | absent (fields 1-4 only) | field 5 | gRParse enhancement; ignore on Docling side |
| Multi-page text fusion | may fuse consecutive pages into one `texts[]` item with multi-page `prov` | one `texts[]` item per page | Compare reading-order concatenated text; pin expected `texts_size` per fixture in the test |
| OCR engines beyond auto/rapidocr | fully typed | typed `UNIMPLEMENTED` | request-option tests |
| `trust_remote_code=true` | accepted for inline | `NOT_IMPLEMENTED` | option test |
| `mineru2` response format | enum present | `NOT_IMPLEMENTED` on VLM convert | option test |
| bbox numeric noise | exact | float | absolute tolerance 1e-3 |

Every other compared field must match exactly under the typed comparison in
`tests/docling_parity_golden_test.cpp`.

## hello-text.pdf (pinned)

- Docling: `texts_size=1` (pages 1 and 2 fused), `pages_size=2`
- gRParse: `texts_size=2` (one item per page), `pages_size=2`
- Reading-order text, page sizes (612x792), tables (0), pictures (0) must match

## Scope of this gate

This test compares committed snapshots. It checks wire compatibility and the
comparison rules, including mutation tests for missing geometry and reordered
body references. It does not run the current parser. A live parse against the
same fixture is still required before claiming parser-output parity. The current
fixture contains no tables or pictures, so their comparison loops provide no
coverage of extraction quality.
