#!/usr/bin/env sh
# Stages the repo-root contract protos into the ai/pipestream/... layout their
# imports use, then generates Python stubs into gen/.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
STAGED="$HERE/.staged-protos"

rm -rf "$STAGED" "$HERE/gen"
mkdir -p "$STAGED/ai/pipestream/document/v1" "$STAGED/ai/pipestream/parse/v1" "$HERE/gen"
cp "$ROOT/document.proto" "$STAGED/ai/pipestream/document/v1/document.proto"
cp "$ROOT/parse_types.proto" "$STAGED/ai/pipestream/parse/v1/parse_types.proto"
cp "$ROOT/parse.proto" "$STAGED/ai/pipestream/parse/v1/parse.proto"
cp "$ROOT/parse_stream.proto" "$STAGED/ai/pipestream/parse/v1/parse_stream.proto"

python -m grpc_tools.protoc -I "$STAGED" \
  --python_out="$HERE/gen" --grpc_python_out="$HERE/gen" \
  "$STAGED/ai/pipestream/document/v1/document.proto" \
  "$STAGED/ai/pipestream/parse/v1/parse_types.proto" \
  "$STAGED/ai/pipestream/parse/v1/parse.proto" \
  "$STAGED/ai/pipestream/parse/v1/parse_stream.proto"

echo "Stubs generated in $HERE/gen"
