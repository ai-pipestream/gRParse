#!/usr/bin/env sh
# Stages the repo-root contract protos into the ai/pipestream/... layout their
# imports use, then generates Python stubs into gen/.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
STAGED="$HERE/.staged-protos"

rm -rf "$STAGED" "$HERE/gen"
mkdir -p "$STAGED/ai/pipestream/document/v1" "$STAGED/ai/pipestream/parse/v1" \
  "$STAGED/ai/pipestream/ebcdic/v1" "$STAGED/lolhtml/v1" \
  "$STAGED/org/apache/opennlp/grpc/v1" "$HERE/gen"
cp "$ROOT/document.proto" "$STAGED/ai/pipestream/document/v1/document.proto"
cp "$ROOT/parse_types.proto" "$STAGED/ai/pipestream/parse/v1/parse_types.proto"
cp "$ROOT/parse.proto" "$STAGED/ai/pipestream/parse/v1/parse.proto"
cp "$ROOT/parse_stream.proto" "$STAGED/ai/pipestream/parse/v1/parse_stream.proto"
# parse_types.proto imports the collector contracts whose typed rules it
# carries (ebcdic_layout, lol_html_options).
cp "$ROOT/collectors/ebcdic.proto" "$STAGED/ai/pipestream/ebcdic/v1/ebcdic.proto"
cp "$ROOT/collectors/lolhtml_types.proto" "$STAGED/lolhtml/v1/types.proto"
cp "$ROOT/collectors/lolhtml_service.proto" "$STAGED/lolhtml/v1/lolhtml_service.proto"
# document.proto imports the OpenNLP analysis document (Document.analyses).
cp "$ROOT/collectors/opennlp_document.proto" "$STAGED/org/apache/opennlp/grpc/v1/opennlp_document.proto"

python -m grpc_tools.protoc -I "$STAGED" \
  --python_out="$HERE/gen" --grpc_python_out="$HERE/gen" \
  "$STAGED/ai/pipestream/ebcdic/v1/ebcdic.proto" \
  "$STAGED/lolhtml/v1/types.proto" \
  "$STAGED/lolhtml/v1/lolhtml_service.proto" \
  "$STAGED/org/apache/opennlp/grpc/v1/opennlp_document.proto" \
  "$STAGED/ai/pipestream/document/v1/document.proto" \
  "$STAGED/ai/pipestream/parse/v1/parse_types.proto" \
  "$STAGED/ai/pipestream/parse/v1/parse.proto" \
  "$STAGED/ai/pipestream/parse/v1/parse_stream.proto"

echo "Stubs generated in $HERE/gen"
