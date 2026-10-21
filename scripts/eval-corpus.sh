#!/usr/bin/env bash
# One command for a corpus eval whose numbers say what produced them.
#
#   scripts/eval-corpus.sh --corpus /data/realdata                 # :latest images, CUDA
#   scripts/eval-corpus.sh --corpus /data/realdata --flavor cpu --tag 0.2.1
#   scripts/eval-corpus.sh --corpus /data/realdata --existing parse-stack --target localhost:50051
#   scripts/eval-corpus.sh --corpus /data/realdata --grparse-image grparse-ci-cpu --flavor cpu \
#       --grparse-commit "$(git rev-parse HEAD)"
#
# Brings the parser stack up from published images (compose project
# grparse-eval, gRPC on EVAL_GRPC_PORT, nothing else published, never built),
# runs eval/corpus/run.py over the corpus, then eval/corpus/analyze.py, and
# prints the clean-output rate beside the SUCCESS rate. Each run lands in
# <out>/corpus/<label>/ with run.json (gRParse commit, every image digest,
# corpus hash and file count, config, UTC times), corpus-manifest.tsv,
# results.jsonl, report.md, analysis.md, and drift.md against the newest
# earlier run over the same corpus hash.
#
# Options:
#   --corpus DIR          local corpus directory (or EVAL_CORPUS_DIR); required
#   --tag TAG             STACK_TAG for every pipestreamai image (default latest)
#   --flavor F            cuda (default), cpu or openvino
#   --grparse-image REF   run this gRParse image (a local build) instead of the tag's
#   --grparse-commit SHA  gRParse commit for an image without a revision label
#   --models-image        fill /models from pipestreamai/grparse-models, not ./models
#   --existing PROJECT    start nothing; evaluate the running compose project
#   --target HOST:PORT    gRParse to dial (default localhost:EVAL_GRPC_PORT, 50151;
#                         localhost:50051 with --existing)
#   --label NAME          run directory name (default <utc>-<flavor>-<tag>)
#   --out DIR             output root (default eval/out)
#   --baseline B          auto (default: newest earlier run, same corpus hash),
#                         none, or a results.jsonl / eval/s3 report.json path
#   --repeat N            parses per document (default 2, as the realdata battery)
#   --workers N           concurrent documents (default 4)
#   --passwords-file F    candidate document passwords, one per line
#   --keep                leave the stack up afterwards
#   --no-pull             use local images instead of pulling the tag again
#   --dry-run             print the commands and run nothing
#   -- ARGS               passed through to eval/corpus/run.py
#
# Environment: EVAL_PROJECT (default grparse-eval), EVAL_GRPC_PORT (50151),
# EVAL_SERVICES (the parsers to start), EVAL_WAIT_SECONDS (600),
# EVAL_PYTHON (default: uv run --with grpcio --with grpcio-tools python),
# EVAL_GATES (analyze.py gates with a baseline, default --fail-on-new-failures).
#
# Exit code: with a baseline, analyze.py's (0 no gate tripped, 1 a gate
# tripped, 2 runs not comparable); without one, run.py's (0 every check
# passed, 1 any document failed a check); 77 when the corpus or gRParse is
# missing. Nothing here greps output.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

usage() { sed -n '2,49p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

corpus=${EVAL_CORPUS_DIR:-}
tag=${STACK_TAG:-latest}
flavor=cuda
grparse_image=
grparse_commit=${EVAL_GRPARSE_COMMIT:-}
models_image=0
existing=
target=
label=
out=$ROOT/eval/out
baseline=auto
repeat=2
workers=4
passwords_file=
keep=0
pull=always
dry_run=0
passthrough=()

need() { [ $# -ge 2 ] || { echo "eval-corpus: $1 needs a value" >&2; exit 2; }; }
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --corpus) need "$@"; corpus=$2; shift 2 ;;
    --tag) need "$@"; tag=$2; shift 2 ;;
    --flavor) need "$@"; flavor=$2; shift 2 ;;
    --grparse-image) need "$@"; grparse_image=$2; shift 2 ;;
    --grparse-commit) need "$@"; grparse_commit=$2; shift 2 ;;
    --models-image) models_image=1; shift ;;
    --existing) need "$@"; existing=$2; shift 2 ;;
    --target) need "$@"; target=$2; shift 2 ;;
    --label) need "$@"; label=$2; shift 2 ;;
    --out) need "$@"; out=$2; shift 2 ;;
    --baseline) need "$@"; baseline=$2; shift 2 ;;
    --repeat) need "$@"; repeat=$2; shift 2 ;;
    --workers) need "$@"; workers=$2; shift 2 ;;
    --passwords-file) need "$@"; passwords_file=$2; shift 2 ;;
    --keep) keep=1; shift ;;
    --no-pull) pull=missing; shift ;;
    --dry-run) dry_run=1; shift ;;
    --) shift; passthrough=("$@"); break ;;
    *) echo "eval-corpus: unknown argument $1 (see --help)" >&2; exit 2 ;;
  esac
done

if [ -z "$corpus" ]; then echo "eval-corpus: --corpus is required" >&2; exit 2; fi
if [ "$dry_run" = 0 ] && [ ! -d "$corpus" ]; then
  echo "eval-corpus: corpus directory $corpus does not exist" >&2
  exit 77
fi
case "$flavor" in cuda|cpu|openvino) ;; *) echo "eval-corpus: --flavor must be cuda, cpu or openvino" >&2; exit 2 ;; esac

project=${EVAL_PROJECT:-grparse-eval}
port=${EVAL_GRPC_PORT:-50151}
if [ -n "$existing" ]; then
  project=$existing
  target=${target:-localhost:50051}
else
  target=${target:-localhost:$port}
fi
label=${label:-$(date -u +%Y%m%dT%H%M%SZ)-$flavor-$tag}
run_dir=$out/corpus/$label
read -r -a python <<<"${EVAL_PYTHON:-uv run --with grpcio --with grpcio-tools python}"
read -r -a gates <<<"${EVAL_GATES:---fail-on-new-failures}"
read -r -a services <<<"${EVAL_SERVICES:-grparse lol-html libreoffice pdfium pdf-inspector epub xml markup ebcdic email calamine}"

run() {
  echo "[eval-corpus] $*" >&2
  if [ "$dry_run" = 0 ]; then "$@"; fi
}

started=0
cleanup() {
  if [ "$started" = 1 ] && [ "$keep" = 0 ]; then
    run "${compose[@]}" --profile parsers --profile calamine down --remove-orphans || true
  fi
  if [ -n "${override:-}" ]; then rm -f "$override"; fi
}
trap cleanup EXIT

if [ -z "$existing" ]; then
  files=(-f compose.stack.yaml)
  case "$flavor" in
    cpu) files+=(-f compose.stack.cpu.yaml) ;;
    openvino) files+=(-f compose.stack.openvino.yaml) ;;
  esac
  if [ "$models_image" = 1 ]; then files+=(-f compose.stack.models.yaml); fi
  files+=(-f eval/corpus/compose.eval.yaml)
  if [ -n "$grparse_image" ]; then
    override=$(mktemp "${TMPDIR:-/tmp}/eval-corpus-override.XXXXXX")
    printf 'services:\n  grparse:\n    image: %s\n    pull_policy: never\n' "$grparse_image" >"$override"
    files+=(-f "$override")
  fi
  compose=(docker compose -p "$project" "${files[@]}")
  export STACK_TAG=$tag EVAL_GRPC_PORT=$port
  started=1
  run "${compose[@]}" --profile parsers --profile calamine up -d --no-build --pull "$pull" "${services[@]}"
fi

run_args=(
  eval/corpus/run.py --corpus-dir "$corpus" --target "$target" --out "$out" --label "$label"
  --repeat "$repeat" --workers "$workers" --compose-project "$project"
  --wait-seconds "${EVAL_WAIT_SECONDS:-600}"
)
if [ -n "$grparse_commit" ]; then run_args+=(--grparse-commit "$grparse_commit"); fi
if [ -n "$passwords_file" ]; then run_args+=(--passwords-file "$passwords_file"); fi
run_args+=("${passthrough[@]}")

set +e
run "${python[@]}" "${run_args[@]}"
run_status=$?
set -e
if [ "$dry_run" = 1 ]; then run_status=0; fi
if [ "$run_status" != 0 ] && [ "$run_status" != 1 ]; then
  echo "[eval-corpus] run.py exit code $run_status; no analysis" >&2
  exit "$run_status"
fi

run "${python[@]}" eval/corpus/analyze.py "$run_dir/results.jsonl" \
  --out-json "$run_dir/analysis.json" --out-md "$run_dir/analysis.md"

status=$run_status
drift_args=()
case "$baseline" in
  none) ;;
  auto) drift_args=("$run_dir/results.jsonl" --baseline-auto "$out/corpus") ;;
  *) drift_args=(--baseline "$baseline" --candidate "$run_dir/results.jsonl") ;;
esac
if [ ${#drift_args[@]} -gt 0 ]; then
  set +e
  run "${python[@]}" eval/corpus/analyze.py "${drift_args[@]}" "${gates[@]}" \
    --out-json "$run_dir/drift.json" --out-md "$run_dir/drift.md"
  status=$?
  set -e
  if [ "$dry_run" = 1 ]; then status=0; fi
fi

if [ "$dry_run" = 0 ]; then
  echo
  sed -n '1,/^## By class/p' "$run_dir/analysis.md" | sed '$d'
  if [ -f "$run_dir/drift.md" ] && head -n 1 "$run_dir/drift.md" | grep -q '^# Corpus drift report'; then
    echo
    sed -n '1,/^## Per-class/p' "$run_dir/drift.md" | sed '$d'
  fi
  echo "[eval-corpus] results in $run_dir (exit $status)"
fi
exit "$status"
