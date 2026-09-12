#!/usr/bin/env bash
# Check the published-image Compose paths without starting or changing a stack.
# --runtime is deliberately explicit and only contacts an existing endpoint.
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mode=preflight
target=
fixture="$project_root/tests/data/report_page.png"
timeout_seconds=${GRPARSE_SMOKE_TIMEOUT_SECONDS:-30}

usage() {
  cat <<'EOF'
Usage: scripts/compose-preflight.sh [--preflight]
       scripts/compose-preflight.sh --runtime --target HOST:PORT [--fixture PATH] [--timeout SECONDS]

Without --runtime, validates the published-image Compose overlay matrix only.
It never pulls images, creates containers, or changes an existing stack.

--runtime contacts the explicitly named, already-running plaintext gRPC endpoint.
It runs Health, GetServiceInfo, hierarchical and hybrid chunk requests against
the checked-in contract, then streams the fixture with grparse-stream-client.
EOF
}

die() {
  local message=$1
  local code=${2:-1}
  echo "compose-preflight: $message" >&2
  exit "$code"
}
need() { command -v "$1" >/dev/null 2>&1 || die "missing required tool: $1" 69; }

while (($#)); do
  case "$1" in
    --preflight) mode=preflight ;;
    --runtime) mode=runtime ;;
    --target) shift; (($#)) || die "--target needs HOST:PORT" 64; target=$1 ;;
    --fixture) shift; (($#)) || die "--fixture needs a path" 64; fixture=$1 ;;
    --timeout) shift; (($#)) || die "--timeout needs seconds" 64; timeout_seconds=$1 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" 64 ;;
  esac
  shift
done

[[ "$timeout_seconds" =~ ^[1-9][0-9]*$ ]] || die "--timeout must be a positive integer" 64

compose_config() {
  local -a files=("$@")
  local -a args=()
  local file
  for file in "${files[@]}"; do args+=(-f "$file"); done
  docker compose "${args[@]}" config --quiet
}

if [[ "$mode" == preflight ]]; then
  need docker
  docker compose version >/dev/null 2>&1 || die "Docker Compose v2 is required" 69
  cd "$project_root"
  declare -a matrices=(
    "base|compose.stack.yaml"
    "cpu|compose.stack.yaml compose.stack.cpu.yaml"
    "openvino|compose.stack.yaml compose.stack.openvino.yaml"
    "arm64-cpu|compose.stack.yaml compose.stack.cpu.yaml compose.stack.arm64.yaml"
    "cpu-models|compose.stack.yaml compose.stack.cpu.yaml compose.stack.models.yaml"
    "openvino-models|compose.stack.yaml compose.stack.openvino.yaml compose.stack.models.yaml"
    "standalone-cpu-models|compose.stack.yaml compose.stack.cpu.yaml compose.stack.standalone.yaml compose.stack.models.yaml"
    "cpu-expose-grpc|compose.stack.yaml compose.stack.cpu.yaml compose.stack.expose-grpc.yaml"
  )
  for matrix in "${matrices[@]}"; do
    IFS='|' read -r label paths <<<"$matrix"
    read -r -a files <<<"$paths"
    echo "== compose config: $label"
    compose_config "${files[@]}"
  done
  echo "compose-preflight: OK (config only; no containers, images, or machine tests run)"
  exit 0
fi

[[ -n "$target" ]] || die "--runtime requires --target HOST:PORT" 64
[[ -f "$fixture" ]] || die "fixture not found: $fixture" 66
need grpcurl

timeout_cmd=
if command -v timeout >/dev/null 2>&1; then
  timeout_cmd=timeout
elif command -v gtimeout >/dev/null 2>&1; then
  timeout_cmd=gtimeout
else
  die "missing required tool: timeout (install coreutils; macOS provides gtimeout)" 69
fi

client=${GRPARSE_STREAM_CLIENT:-}
if [[ -z "$client" ]]; then
  for candidate in "$project_root/build/grparse-stream-client" "$project_root/cmake-build-release/grparse-stream-client" /usr/local/bin/grparse-stream-client; do
    [[ -x "$candidate" ]] && { client=$candidate; break; }
  done
fi
[[ -n "$client" && -x "$client" ]] || die "grparse-stream-client was not found; set GRPARSE_STREAM_CLIENT" 69

proto_root=$(mktemp -d "${TMPDIR:-/tmp}/grparse-contract.XXXXXX")
trap 'rm -rf "$proto_root"' EXIT
mkdir -p "$proto_root/ai/pipestream/document/v1" "$proto_root/ai/pipestream/parse/v1"
cp "$project_root/document.proto" "$proto_root/ai/pipestream/document/v1/document.proto"
cp "$project_root/parse_types.proto" "$proto_root/ai/pipestream/parse/v1/parse_types.proto"
cp "$project_root/parse.proto" "$proto_root/ai/pipestream/parse/v1/parse.proto"

grpc() {
  local method=$1
  local data=$2
  grpcurl -plaintext -max-time "$timeout_seconds" -import-path "$proto_root" \
    -proto "$proto_root/ai/pipestream/parse/v1/parse.proto" -d "$data" "$target" "$method"
}

base64_fixture=$(base64 <"$fixture" | tr -d '\n')
request=$(printf '{"request":{"sources":[{"file":{"base64String":"%s","filename":"%s"}}],"includeConvertedDoc":false,"chunkingOptions":{"includeRawText":true}}}' "$base64_fixture" "$(basename "$fixture")")

echo "== runtime: Health"
grpc ai.pipestream.parse.v1.ParseService/Health '{}'
echo "== runtime: GetServiceInfo"
grpc ai.pipestream.parse.v1.ParseService/GetServiceInfo '{}'
echo "== runtime: ChunkHierarchicalSource"
grpc ai.pipestream.parse.v1.ParseService/ChunkHierarchicalSource "$request" | grep -q 'rulesDigest' || die "hierarchical chunk response contains no rulesDigest"
echo "== runtime: ChunkHybridSource"
grpc ai.pipestream.parse.v1.ParseService/ChunkHybridSource "$request" | grep -q 'rulesDigest' || die "hybrid chunk response contains no rulesDigest"
echo "== runtime: streamed parse"
"$timeout_cmd" "$timeout_seconds" "$client" "$fixture" "$target" | tee /dev/stderr | grep -q '^complete ' || die "streamed parse did not emit a complete event"
echo "embedding check: skipped (the checked-in published ParseService contract advertises no embedding RPC)"
echo "compose-preflight: OK (existing endpoint only: $target)"
