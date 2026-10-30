#!/usr/bin/env sh
# Bootstrap or update a gRParse multi-repo workspace (`grpc-services`).
#
# compose.stack.yaml builds every sibling service from ../<repo> and mounts
# grpc-asr/models into the asr service, so the sibling checkouts must exist
# next to gRParse when building or developing locally.
#
# This script works in two modes:
#   1. From inside an existing gRParse checkout:
#        ./compose/clone-siblings.sh [--full] [--pull]
#      Clones all sibling repositories into the parent directory of gRParse.
#
#   2. Standalone on a brand-new machine (before gRParse is cloned):
#        curl -fsSL https://raw.githubusercontent.com/ai-pipestream/gRParse/main/compose/clone-siblings.sh | sh
#        curl -fsSL https://raw.githubusercontent.com/ai-pipestream/gRParse/main/compose/clone-siblings.sh | sh -s -- --full ./grpc-services
#      Creates the target workspace directory (default: ./grpc-services) and
#      clones gRParse + all sibling services + parser-protos into it.
#
# Environment variables:
#   GIT_BASE       Base git URL (default: https://github.com/ai-pipestream)
#   FORGEJO_BASE   Optional primary Forgejo base URL (e.g. https://git.rokkon.com/ai-pipestream)
#                  when --dual-remotes is used.
#   WORKSPACE_DIR  Target workspace directory when run standalone.
#   FULL_CLONE     Set to 1 for full git history (same as --full).
set -eu

GIT_BASE="${GIT_BASE:-https://github.com/ai-pipestream}"
FORGEJO_BASE="${FORGEJO_BASE:-https://git.rokkon.com/ai-pipestream}"
FULL_CLONE="${FULL_CLONE:-0}"
PULL_EXISTING=0
DUAL_REMOTES=0
TARGET_DIR=""

usage() {
  cat <<'EOF'
Usage: clone-siblings.sh [OPTIONS] [WORKSPACE_DIR]

Checkout or update gRParse and all sibling grpc-services in one workspace.

Options:
  -w, --workspace DIR   Target workspace directory (default: parent of gRParse
                        checkout, or ./grpc-services when run standalone)
      --full            Clone full git history instead of --depth 1
      --shallow         Clone with --depth 1 (default)
  -p, --pull, --update  Fast-forward existing checkouts from origin
      --dual-remotes    Configure Forgejo as `origin` and GitHub as `github`
  -h, --help            Show this help message

Examples:
  # Bootstrap a full workspace on a new machine:
  curl -fsSL https://raw.githubusercontent.com/ai-pipestream/gRParse/main/compose/clone-siblings.sh | sh -s -- --full ./grpc-services

  # From inside an existing gRParse checkout:
  ./compose/clone-siblings.sh
  ./compose/clone-siblings.sh --full --pull

  # Custom git server / SSH base:
  GIT_BASE=git@github.com:ai-pipestream ./compose/clone-siblings.sh --full
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    -w|--workspace)
      if [ $# -lt 2 ]; then
        echo "error: $1 requires a directory argument" >&2
        exit 1
      fi
      TARGET_DIR="$2"
      shift 2
      ;;
    --full)
      FULL_CLONE=1
      shift
      ;;
    --shallow)
      FULL_CLONE=0
      shift
      ;;
    -p|--pull|--update)
      PULL_EXISTING=1
      shift
      ;;
    --dual-remotes)
      DUAL_REMOTES=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "error: unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      if [ -n "$TARGET_DIR" ]; then
        echo "error: multiple workspace directories specified ('$TARGET_DIR' and '$1')" >&2
        exit 1
      fi
      TARGET_DIR="$1"
      shift
      ;;
  esac
done

if [ $# -gt 0 ]; then
  if [ -n "$TARGET_DIR" ]; then
    echo "error: unexpected extra arguments: $*" >&2
    exit 1
  fi
  TARGET_DIR="$1"
fi

script_dir=""
case "$0" in
  */*)
    if [ -f "$0" ]; then
      script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
    fi
    ;;
esac

in_grparse_checkout=0
here=""
if [ -n "$script_dir" ] && [ -f "$script_dir/../compose.stack.yaml" ] && [ -f "$script_dir/../release/fleet.tsv" ]; then
  in_grparse_checkout=1
  here="$(CDPATH= cd -- "$script_dir/.." && pwd)"
fi

if [ -n "$TARGET_DIR" ]; then
  mkdir -p "$TARGET_DIR"
  parent="$(CDPATH= cd -- "$TARGET_DIR" && pwd)"
elif [ -n "${WORKSPACE_DIR:-}" ]; then
  mkdir -p "$WORKSPACE_DIR"
  parent="$(CDPATH= cd -- "$WORKSPACE_DIR" && pwd)"
elif [ "$in_grparse_checkout" -eq 1 ]; then
  parent="$(dirname -- "$here")"
else
  mkdir -p "./grpc-services"
  parent="$(CDPATH= cd -- "./grpc-services" && pwd)"
fi

default_branch_for() {
  case "$1" in
    grpc-lol-html) echo "master" ;;
    *)             echo "main" ;;
  esac
}

# All repositories in the gRParse stack (gRParse itself + collectors + shell
# peers + PDF backends + the shared PdfBackendService proto contract).
repos="gRParse \
grpc-lol-html grpc-libreoffice grpc-calamine grpc-pdf-inspector \
grpc-epub grpc-xml grpc-markup grpc-ebcdic grpc-email grpc-enrich \
grpc-asr grpc-vlm-convert fastwarc-grpc grpc-pdfium grpc-qparse \
grpc-poppler parser-protos"

clone_base="$GIT_BASE"
if [ "$DUAL_REMOTES" -eq 1 ]; then
  clone_base="$FORGEJO_BASE"
fi

echo "Workspace: $parent"
echo "Source:    $clone_base"
echo

for repo in $repos; do
  branch="$(default_branch_for "$repo")"
  dest="$parent/$repo"

  if [ -e "$dest" ]; then
    if [ "$PULL_EXISTING" -eq 1 ] && { [ -d "$dest/.git" ] || [ -f "$dest/.git" ]; }; then
      if [ "$FULL_CLONE" -eq 1 ] && [ "$(git -C "$dest" rev-parse --is-shallow-repository 2>/dev/null || echo false)" = "true" ]; then
        echo "unshallowing $dest"
        git -C "$dest" fetch --unshallow origin
      fi
      current_branch="$(git -C "$dest" branch --show-current 2>/dev/null || true)"
      if [ "$current_branch" = "$branch" ]; then
        echo "updating $dest ($branch)"
        git -C "$dest" pull --ff-only origin "$branch"
      else
        echo "skipping $dest (on branch '${current_branch:-detached}', expected '$branch')"
      fi
    else
      echo "exists   $dest"
    fi
  else
    echo "cloning  $clone_base/$repo ($branch)"
    if [ "$FULL_CLONE" -eq 1 ]; then
      git clone --branch "$branch" "$clone_base/$repo.git" "$dest"
    else
      git clone --depth 1 --branch "$branch" "$clone_base/$repo.git" "$dest"
    fi
  fi

  if [ "$DUAL_REMOTES" -eq 1 ] && { [ -d "$dest/.git" ] || [ -f "$dest/.git" ]; }; then
    git -C "$dest" remote set-url origin "$FORGEJO_BASE/$repo.git"
    if git -C "$dest" remote get-url github >/dev/null 2>&1; then
      git -C "$dest" remote set-url github "$GIT_BASE/$repo.git"
    else
      git -C "$dest" remote add github "$GIT_BASE/$repo.git"
    fi
  fi
done

# The asr service mounts grpc-asr/models read-only; compose refuses to
# start when a bind-mount source is missing, so make sure it exists even
# before any whisper weights are downloaded into it.
mkdir -p "$parent/grpc-asr/models"

echo
echo "Done. Workspace is ready at: $parent"
echo "gRParse's own models are separate: run '$parent/gRParse/scripts/fetch-models.sh'"
echo "(or use compose.stack.models.yaml) before bringing the stack up."
