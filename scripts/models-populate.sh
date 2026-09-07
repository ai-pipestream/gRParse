#!/usr/bin/env bash
# Entry point of the pipestreamai/grparse-models image: copy the verified
# model files baked under /models into a target directory (a volume mounted
# into this init container) and verify the copy against the bundled manifest.
#
#   models-populate.sh [TARGET]          default /target
#
# A file whose sha256 already matches the source is left alone, so a second
# run on a populated volume copies nothing. Exit 0 only when every file the
# image carries is present in the target with the manifest's hash.
#
# Runs on the hardened base without awk or sed: bash and coreutils only.
set -euo pipefail

source=${MODELS_SOURCE:-/models}
target=${1:-/target}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
verify=$script_dir/fetch-models.sh

sha_of() { sha256sum "$1" | cut -c1-64; }

copy_missing() {
  local file rel copied=0 kept=0
  while IFS= read -r file; do
    rel=${file#"$source"/}
    [ "$rel" = MANIFEST ] && continue
    if [ -f "$target/$rel" ] && [ "$(sha_of "$file")" = "$(sha_of "$target/$rel")" ]; then
      kept=$((kept + 1))
      continue
    fi
    mkdir -p "$(dirname "$target/$rel")"
    cp "$file" "$target/$rel.part"
    mv "$target/$rel.part" "$target/$rel"
    copied=$((copied + 1))
  done < <(find "$source" -type f | sort)
  echo "populate: $copied file(s) copied, $kept already in place at $target"
}

# The image is built for one layout selection; verify the same selection.
layout_flag() {
  if [ -f "$source/layout_heron.onnx" ] && [ -f "$source/layout_publaynet.onnx" ]; then
    echo all
  elif [ -f "$source/layout_publaynet.onnx" ]; then
    echo picodet
  else
    echo heron
  fi
}

main() {
  [ -d "$source" ] || { echo "no model source at $source" >&2; exit 1; }
  [ -r "$source/MANIFEST" ] || { echo "no MANIFEST under $source" >&2; exit 1; }
  mkdir -p "$target"
  copy_missing
  local -a args=(--verify --dir "$target" --manifest "$source/MANIFEST" --layout "$(layout_flag)")
  [ -f "$source/chunk/tokenizer.json" ] && args+=(--tokenizer)
  "$verify" "${args[@]}"
}

main "$@"
