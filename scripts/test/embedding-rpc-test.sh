#!/usr/bin/env bash
set -euo pipefail

# Run on the model host; all containers are isolated and images must be cached.
image=${EMBEDDING_RPC_IMAGE:-grparse:local-embeddings-cpu}
backend=${EMBEDDING_RPC_BACKEND:-cpu}
startup_seconds=${EMBEDDING_RPC_STARTUP_SECONDS:-300}
if [[ ! $startup_seconds =~ ^[1-9][0-9]{0,2}$ ]] || ((startup_seconds > 900)); then
  echo 'EMBEDDING_RPC_STARTUP_SECONDS must be a positive integer <=900' >&2
  exit 64
fi
case $backend in
  cpu|openvino|tensorrt) ;;
  *) echo 'EMBEDDING_RPC_BACKEND must be cpu, openvino, or tensorrt' >&2; exit 64 ;;
esac
client_image=fullstorydev/grpcurl:latest
# --deadline-only runs just warmup, one short-deadline RPC, and recovery.
deadline_only=false
if [[ ${1:-} == --deadline-only ]]; then deadline_only=true; shift; fi
fixture=${1:-$HOME/builds/local-embeddings/tests/golden/corpus/hello-text.pdf}
models=${EMBEDDING_RPC_OCR_MODELS:-$HOME/grparse-models}
embedding_models=${EMBEDDING_RPC_EMBEDDING_MODELS:-$HOME/builds/embedding-models}
for tool in docker jq base64 timeout cmp; do command -v "$tool" >/dev/null; done
test -f "$fixture"
test -d "$models"
test -d "$embedding_models"
docker image inspect "$image" "$client_image" >/dev/null
artifacts=$(mktemp -d "$HOME/builds/embedding-rpc.XXXXXXXX")
suffix=${artifacts##*.}
server=embedding-rpc-server-$suffix
client=embedding-rpc-client-$suffix
cleanup() {
  result=$?
  trap - EXIT
  docker logs "$server" >"$artifacts/server-final.log" 2>&1 || true
  docker rm -f "$client" "$server" >/dev/null 2>&1 || true
  echo "embedding-rpc-test: exit=$result logs=$artifacts"
  exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
echo "image=$(docker image inspect --format '{{.Id}}' "$image") logs=$artifacts"

rpc() {
  local method=$1 input=$2 output=$3
  local max_time=${4:-90}
  shift 3
  if (( $# > 0 )); then shift; fi
  timeout "${RPC_TIMEOUT_SECONDS:-100}" docker run --rm --pull=never --name "$client" -i \
    --user "$(id -u):$(id -g)" \
    -v "$artifacts:/rpc-test" --network "container:$server" "$client_image" \
    -plaintext -connect-timeout 5 -max-time "$max_time" "$@" -d @ \
    localhost:50051 "ai.pipestream.parse.v1.ParseService/$method" \
    <"$input" >"$output" 2>"$output.stderr"
}

start_server() {
  local mode=$1
  local memory=4g node deadline remaining probe_seconds
  local -a extra=()
  if [[ $mode != off ]]; then
    extra=(-v "$embedding_models:/embedding-models:ro" \
      -e GRPARSE_EMBEDDING_MODEL_DIR=/embedding-models)
  fi
  case $mode in
    openvino)
      memory=8g
      extra+=(--device /dev/dri:/dev/dri -e GRPARSE_EMBEDDING_DEVICE=GPU)
      # Use the host render-node group IDs, not image-specific group names.
      local -a render_nodes=(/dev/dri/renderD*)
      [[ -e ${render_nodes[0]} ]] || return 1
      for node in "${render_nodes[@]}"; do
        extra+=(--group-add "$(stat -c %g "$node")")
      done
      ;;
    tensorrt)
      memory=8g
      extra+=(--gpus device=0 -e GRPARSE_EMBEDDING_GPU_INDEX=0)
      ;;
    cpu|off)
      extra+=(-e NVIDIA_VISIBLE_DEVICES=void)
      ;;
  esac
  docker run -d --pull=never --name "$server" --read-only --tmpfs /tmp \
    --cpus 2 --memory "$memory" --cap-drop ALL \
    -v "$models:/models:ro" -e GRPARSE_ORT_EP=cpu \
    -e GRPARSE_PAGE_WORKERS=1 -e GRPARSE_INTRA_OP_THREADS=2 \
    -e GRPARSE_UNARY_WORKERS=2 -e "GRPARSE_EMBEDDING_BACKEND=$mode" \
    "${extra[@]}" "$image" >/dev/null
  deadline=$((SECONDS + startup_seconds))
  while ((SECONDS < deadline)); do
    [[ $(docker inspect --format '{{.State.Running}}' "$server") == true ]] || return 1
    remaining=$((deadline - SECONDS))
    ((remaining > 0)) || break
    probe_seconds=$remaining
    ((probe_seconds <= 5)) || probe_seconds=5
    if RPC_TIMEOUT_SECONDS=$remaining rpc GetServiceInfo "$artifacts/empty.json" \
        "$artifacts/$mode-info.json" "$probe_seconds"; then
      echo "$mode server ready"
      return
    fi
    [[ $(docker inspect --format '{{.State.Running}}' "$server") == true ]] || return 1
    remaining=$((deadline - SECONDS))
    ((remaining > 0)) || break
    if ((remaining < 2)); then sleep "$remaining"; else sleep 2; fi
  done
  return 1
}

expect_error() {
  local method=$1 input=$2 output=$3 code=$4
  local status=0
  rpc "$method" "$input" "$output" || status=$?
  test "$status" -ne 0
  test "$status" -ne 124
  test ! -s "$output"
  grep -F "Code: $code" "$output.stderr"
}

stable() {
  jq -S '{chunks: [.response.chunks[] | del(.embedding)], documents: .response.documents}' "$1"
}

verify_deadline() {
  local request=$artifacts/deadline-request.json
  local status=0
  jq '.request.chunkingOptions = {maxTokens: 64} |
    .request.embeddingOptions = {enabled: true}' "$artifacts/base.json" >"$request"
  rpc ChunkHybridSource "$request" "$artifacts/deadline-warm.json"
  jq -e '(.response.chunks | length) > 0 and
    all(.response.chunks[]; (.embedding.values | length) == 384)' \
    "$artifacts/deadline-warm.json" >/dev/null
  timeout 100 docker run --rm --pull=never --name "$client" \
    --user "$(id -u):$(id -g)" -v "$artifacts:/rpc-test" \
    --network "container:$server" "$client_image" \
    -plaintext -connect-timeout 5 -max-time 90 \
    -protoset-out /rpc-test/deadline-schema.pb localhost:50051 \
    describe ai.pipestream.parse.v1.ParseService \
    >"$artifacts/deadline-schema.txt" 2>"$artifacts/deadline-schema.stderr"
  test -s "$artifacts/deadline-schema.pb"
  # Use the captured schema so the short deadline cannot expire in reflection.
  rpc ChunkHybridSource "$request" "$artifacts/deadline.json" 0.05 \
    -protoset /rpc-test/deadline-schema.pb || status=$?
  test "$status" -ne 0
  test "$status" -ne 124
  test ! -s "$artifacts/deadline.json"
  grep -F 'Code: DeadlineExceeded' "$artifacts/deadline.json.stderr"
  echo "deadline client exit=$status stdout_bytes=0"
  rpc ChunkHybridSource "$request" "$artifacts/deadline-recovery.json"
  jq -e '(.response.chunks | length) > 0 and
    all(.response.chunks[]; (.embedding.values | length) == 384)' \
    "$artifacts/deadline-recovery.json" >/dev/null
  stable "$artifacts/deadline-warm.json" >"$artifacts/deadline-warm-stable.json"
  stable "$artifacts/deadline-recovery.json" >"$artifacts/deadline-recovery-stable.json"
  cmp "$artifacts/deadline-warm-stable.json" "$artifacts/deadline-recovery-stable.json"
  echo 'PASS RPC-level deadline: no partial JSON; subsequent enabled request succeeds'
}

printf '{}\n' >"$artifacts/empty.json"
base64 -w0 "$fixture" >"$artifacts/source.b64"
jq -n --rawfile data "$artifacts/source.b64" \
  '{request: {sources: [{file: {filename: "hello-text.pdf", base64String: $data}}],
    includeConvertedDoc: true}}' >"$artifacts/base.json"
start_server "$backend"
jq -e --arg backend "$backend" '.embeddings.available == true and .embeddings.model.backend == $backend and
  .embeddings.model.dimensions == 384 and
  .embeddings.model.revision == "826711e54e001c83835913827a843d8dd0a1def9"' \
  "$artifacts/$backend-info.json" >/dev/null
verify_deadline
if [[ $deadline_only == true ]]; then
  echo 'embedding-rpc-test: deadline-only PASS'
  exit 0
fi
for method in ChunkHybridSource ChunkHierarchicalSource; do
  jq --arg method "$method" 'if $method == "ChunkHybridSource" then
    .request.chunkingOptions = {maxTokens: 64} else . end' \
    "$artifacts/base.json" >"$artifacts/$method-off-request.json"
  jq '.request.embeddingOptions = {enabled: true}' \
    "$artifacts/$method-off-request.json" >"$artifacts/$method-on-request.json"
  rpc "$method" "$artifacts/$method-off-request.json" "$artifacts/$method-off.json"
  rpc "$method" "$artifacts/$method-on-request.json" "$artifacts/$method-on.json"
  jq -e '(.response.chunks | length) > 0 and
    all(.response.chunks[]; has("embedding") | not) and
    (.response.documents | length) > 0' "$artifacts/$method-off.json" >/dev/null
  jq -e --arg backend "$backend" 'all(.response.chunks[];
    .embedding.embedded_text == .text and
    .embedding.text_mode == "EMBEDDING_TEXT_MODE_TEXT" and
    (.embedding.values | length) == 384 and
    .embedding.model.dimensions == 384 and .embedding.model.backend == $backend and
    .embedding.model.model_id == "sentence-transformers/all-MiniLM-L6-v2" and
    .embedding.model.revision == "826711e54e001c83835913827a843d8dd0a1def9" and
    (([.embedding.values[] | . * .] | add | sqrt) as $norm |
      $norm >= 0.99999 and $norm <= 1.00001))' "$artifacts/$method-on.json" >/dev/null
  stable "$artifacts/$method-off.json" >"$artifacts/$method-off-stable.json"
  stable "$artifacts/$method-on.json" >"$artifacts/$method-on-stable.json"
  cmp "$artifacts/$method-off-stable.json" "$artifacts/$method-on-stable.json"
  jq '.request.embeddingOptions.modelId = "invalid-model"' \
    "$artifacts/$method-on-request.json" >"$artifacts/$method-bad-request.json"
  expect_error "$method" "$artifacts/$method-bad-request.json" \
    "$artifacts/$method-bad.json" InvalidArgument
  echo "PASS $method: off/on stable chunks and documents, vectors, bad model"
done
docker logs "$server" >"$artifacts/server-$backend.log" 2>&1
docker rm -f "$server" >/dev/null
start_server off
jq -e '(.embeddings.available // false) == false' "$artifacts/off-info.json" >/dev/null
for method in ChunkHybridSource ChunkHierarchicalSource; do
  rpc "$method" "$artifacts/$method-off-request.json" "$artifacts/$method-disabled.json"
  stable "$artifacts/$method-disabled.json" >"$artifacts/$method-disabled-stable.json"
  cmp "$artifacts/$method-off-stable.json" "$artifacts/$method-disabled-stable.json"
  jq -e 'all(.response.chunks[]; has("embedding") | not)' \
    "$artifacts/$method-disabled.json" >/dev/null
  expect_error "$method" "$artifacts/$method-on-request.json" \
    "$artifacts/$method-unavailable.json" FailedPrecondition
  echo "PASS $method: server disabled without embedding mount, original output preserved"
done
echo 'embedding-rpc-test: PASS'
