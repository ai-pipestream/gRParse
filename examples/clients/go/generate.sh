#!/usr/bin/env sh
# Stages the repo-root contract protos into the ai/pipestream/... layout their
# imports use, then generates Go stubs into gen/.  The protos carry no
# go_package option, so M flags map every file into this module.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
STAGED="$HERE/.staged-protos"
MODULE=github.com/ai-pipestream/gRParse/examples/clients/go

rm -rf "$STAGED" "$HERE/gen"
mkdir -p "$STAGED/ai/pipestream/document/v1" "$STAGED/ai/pipestream/parse/v1"
cp "$ROOT/document.proto" "$STAGED/ai/pipestream/document/v1/document.proto"
cp "$ROOT/parse_types.proto" "$STAGED/ai/pipestream/parse/v1/parse_types.proto"
cp "$ROOT/parse.proto" "$STAGED/ai/pipestream/parse/v1/parse.proto"
cp "$ROOT/parse_stream.proto" "$STAGED/ai/pipestream/parse/v1/parse_stream.proto"

MAPPINGS="Mai/pipestream/document/v1/document.proto=$MODULE/gen/documentv1"
MAPPINGS="$MAPPINGS,Mai/pipestream/parse/v1/parse_types.proto=$MODULE/gen/parsev1"
MAPPINGS="$MAPPINGS,Mai/pipestream/parse/v1/parse.proto=$MODULE/gen/parsev1"
MAPPINGS="$MAPPINGS,Mai/pipestream/parse/v1/parse_stream.proto=$MODULE/gen/parsev1"

protoc -I "$STAGED" \
  --go_out="$HERE" --go_opt=module="$MODULE","$MAPPINGS" \
  --go-grpc_out="$HERE" --go-grpc_opt=module="$MODULE","$MAPPINGS" \
  "$STAGED/ai/pipestream/document/v1/document.proto" \
  "$STAGED/ai/pipestream/parse/v1/parse_types.proto" \
  "$STAGED/ai/pipestream/parse/v1/parse.proto" \
  "$STAGED/ai/pipestream/parse/v1/parse_stream.proto"

echo "Stubs generated in $HERE/gen"
