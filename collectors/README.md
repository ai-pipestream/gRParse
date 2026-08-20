# Vendored collector contracts

Each file here is copied byte-identical from its collector repository and is
never edited in this repo; the collector owns its wire contract. To update,
re-copy the file and rebuild.

| File | Source of truth |
|---|---|
| `asr_service.proto` | grpc-asr `proto/ai/pipestream/asr/v1/asr_service.proto` |
| `email_service.proto` | grpc-email `proto/ai/pipestream/email/v1/email_service.proto` |
| `xml.proto`, `xml_service.proto` | grpc-xml `proto/ai/pipestream/xml/v1/` |
| `ebcdic.proto`, `ebcdic_service.proto` | grpc-ebcdic `proto/ai/pipestream/ebcdic/v1/` |
| `epub_types.proto`, `epub_service.proto` | grpc-epub `proto/ai/pipestream/epub/v1/` (`types.proto` is renamed here to stay unambiguous; it stages back to `ai/pipestream/epub/v1/types.proto`) |
| `markup.proto`, `markup_service.proto` | grpc-markup `proto/ai/pipestream/markup/v1/` |
| `lolhtml_types.proto`, `lolhtml_service.proto` | grpc-lol-html `proto/lolhtml/v1/` (`types.proto` is renamed here to stay unambiguous; it stages back to `lolhtml/v1/types.proto`) |
| `warc.proto`, `warc_service.proto` | chatnoir-resiliparse fastwarc-grpc `proto/fastwarc/v1/` (stages back to `fastwarc/v1/`) |

Every one of these except the lol-html and fastwarc pairs imports
`ai/pipestream/document/v1/document.proto`, which
resolves to this repository's own `document.proto` — the fleet-wide source of
truth the collectors vendored in the first place. If a copy stops compiling
against it, the collector's vendored document.proto has drifted; fix that
there, not here.
