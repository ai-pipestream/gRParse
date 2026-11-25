# Vendored document repository contract

The files under this directory are copied byte-identical from
[protomolt](https://github.com/ai-pipestream/protomolt) and are never edited
here; protomolt owns the wire contract. They keep their own import paths, so
`repository/ai/protomolt/proto/repo/v2/document_service.proto` is imported as
`ai/protomolt/proto/repo/v2/document_service.proto`.

gRParse uses one service from them: `ai.protomolt.proto.repo.v2.DocumentService`,
the parts-based document repository, which the repository target
(`src/targets/repository_target.h`, `RepositoryTarget` in `parse_types.proto`)
saves a conversion through. The target is an add-on: the stubs are compiled in,
and nothing dials a repository unless a request names the target and
`GRPARSE_REPOSITORY_TARGET` says where it is.

| Files | Source of truth in protomolt |
|---|---|
| `ai/protomolt/proto/repo/v2/` (`document_service`, `errors`, `events`, `manifest`), `ai/protomolt/proto/repo/v1/` (`address`, `any_resolution`, `document`, `document_publication`, `manifest`, `schema_asset_reference`, `schema_occurrence`, `security`), `ai/protomolt/proto/repo/semantic/v1/semantic.proto` | `repo/proto/src/main/proto/` |
| `ai/protomolt/proto/projection/v1/projection.proto` | `transform/projection/src/main/proto/` |
| `ai/protomolt/proto/types/v1/concepts.proto` | `protobuf/types/src/main/proto/` |
| `ai/protomolt/proto/validate/v1/validate.proto` | `protobuf/validation/src/main/proto/` |
| `ai/protomolt/proto/meta/v1/metadata.proto` | `protobuf/metadata/src/main/proto/` |
| `buf/validate/validate.proto` | `protobuf/validation-protovalidate/src/main/proto/` (protomolt's own vendored copy of the Buf protovalidate options, Apache-2.0) |

Only `document_service.proto` is used directly; the other 17 are its import
closure, which protoc needs whole.

Every file has a row in `collectors/PINS.tsv` (`repo` `protomolt`, its path
there, the commit it was copied at), and `scripts/test/vendored-protos-test.sh`
fails on any byte of difference, the same rule the collector contracts follow
(`collectors/README.md`). To update, re-copy the files from one protomolt
commit and move all 18 pins to it in the same change. A new import in
`document_service.proto` means a new file here, a new row there and a new
`configure_file` line in the repository block of `CMakeLists.txt`.
