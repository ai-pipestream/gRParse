#!/usr/bin/env sh
# compose.stack.yaml bind-mounts the sibling repos' proto directories into
# the demo shell (and grpc-asr/models into the asr service), so the sibling
# checkouts must exist next to this gRParse checkout. On a fresh machine
# this script shallow-clones every sibling the stack references; existing
# checkouts are left untouched.
#
#   ./compose/clone-siblings.sh            # clone from github.com/ai-pipestream
#   GIT_BASE=git@git.example.com:org ./compose/clone-siblings.sh
set -eu

GIT_BASE="${GIT_BASE:-https://github.com/ai-pipestream}"
here="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
parent="$(dirname -- "$here")"

repos="grpc-lol-html grpc-libreoffice grpc-calamine grpc-pdf-inspector \
grpc-epub grpc-xml grpc-markup grpc-ebcdic grpc-email grpc-enrich \
grpc-asr grpc-vlm-convert fastwarc-grpc grPOIc"

for repo in $repos; do
  if [ -e "$parent/$repo" ]; then
    echo "exists   $parent/$repo"
  else
    echo "cloning  $GIT_BASE/$repo"
    git clone --depth 1 "$GIT_BASE/$repo.git" "$parent/$repo"
  fi
done

# The asr service mounts grpc-asr/models read-only; compose refuses to
# start when a bind-mount source is missing, so make sure it exists even
# before any whisper weights are downloaded into it.
mkdir -p "$parent/grpc-asr/models"

echo
echo "Done. gRParse's own models are separate: see models/README.md for the"
echo "four ONNX model downloads before bringing the stack up."
