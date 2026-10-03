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
mkdir -p "$STAGED/ai/pipestream/document/v1" "$STAGED/ai/pipestream/parse/v1" \
  "$STAGED/ai/pipestream/ebcdic/v1" "$STAGED/lolhtml/v1"
cp "$ROOT/document.proto" "$STAGED/ai/pipestream/document/v1/document.proto"
cp "$ROOT/parse_types.proto" "$STAGED/ai/pipestream/parse/v1/parse_types.proto"
cp "$ROOT/parse.proto" "$STAGED/ai/pipestream/parse/v1/parse.proto"
cp "$ROOT/parse_stream.proto" "$STAGED/ai/pipestream/parse/v1/parse_stream.proto"
# parse_types.proto imports the collector contracts whose typed rules it
# carries (ebcdic_layout, lol_html_options).
cp "$ROOT/collectors/ebcdic.proto" "$STAGED/ai/pipestream/ebcdic/v1/ebcdic.proto"
cp "$ROOT/collectors/lolhtml_types.proto" "$STAGED/lolhtml/v1/types.proto"
cp "$ROOT/collectors/lolhtml_service.proto" "$STAGED/lolhtml/v1/lolhtml_service.proto"

MAPPINGS="Mai/pipestream/document/v1/document.proto=$MODULE/gen/documentv1"
MAPPINGS="$MAPPINGS,Mai/pipestream/ebcdic/v1/ebcdic.proto=$MODULE/gen/ebcdicv1"
MAPPINGS="$MAPPINGS,Mlolhtml/v1/types.proto=$MODULE/gen/lolhtmlv1"
MAPPINGS="$MAPPINGS,Mlolhtml/v1/lolhtml_service.proto=$MODULE/gen/lolhtmlv1"
MAPPINGS="$MAPPINGS,Mai/pipestream/parse/v1/parse_types.proto=$MODULE/gen/parsev1"
MAPPINGS="$MAPPINGS,Mai/pipestream/parse/v1/parse.proto=$MODULE/gen/parsev1"
MAPPINGS="$MAPPINGS,Mai/pipestream/parse/v1/parse_stream.proto=$MODULE/gen/parsev1"

protoc -I "$STAGED" \
  --go_out="$HERE" --go_opt=module="$MODULE","$MAPPINGS" \
  --go-grpc_out="$HERE" --go-grpc_opt=module="$MODULE","$MAPPINGS" \
  "$STAGED/ai/pipestream/ebcdic/v1/ebcdic.proto" \
  "$STAGED/lolhtml/v1/types.proto" \
  "$STAGED/lolhtml/v1/lolhtml_service.proto" \
  "$STAGED/ai/pipestream/document/v1/document.proto" \
  "$STAGED/ai/pipestream/parse/v1/parse_types.proto" \
  "$STAGED/ai/pipestream/parse/v1/parse.proto" \
  "$STAGED/ai/pipestream/parse/v1/parse_stream.proto"

echo "Stubs generated in $HERE/gen"
