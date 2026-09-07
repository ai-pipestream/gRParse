#!/usr/bin/env bash
# Fetch, patch and verify the model files gRParse loads from GRPARSE_MODELS_DIR.
#
#   scripts/fetch-models.sh [--dir DIR] [--layout heron|picodet|all] [--tokenizer]
#   scripts/fetch-models.sh --verify [--dir DIR] [--layout ...] [--tokenizer]
#
# models/MANIFEST is the source of truth: every file's sha256, size, group,
# license and upstream URL. A download is kept only when its sha256 matches;
# a file already on disk with the right hash is left alone, so the script is
# idempotent and a partial run resumes where it stopped (curl continues a
# .part file). The figure classifier is derived, not downloaded: the
# published export is fetched under its own name and
# scripts/patch_figure_classifier.py writes the graph the server loads.
#
# --verify touches nothing. It exits 0 when every present file matches the
# manifest and every required (ocr) file is present; it exits 1 naming each
# mismatch or missing required file. Optional files that are absent are
# listed as such and do not fail the check.
#
# Environment:
#   FETCH_MODELS_PYTHON   command prefix that runs the patch script with the
#                         onnx module importable; default is `uv run --with
#                         onnx --with onnxruntime --with numpy python` when uv
#                         is installed, else `python3` (must import onnx).
#   FETCH_MODELS_MANIFEST alternative manifest (the tests use one with
#                         file:// URLs); --manifest does the same.
#
# Needs bash, coreutils (sha256sum, stat, mktemp) and curl for downloads.
# Exit codes: 0 ok, 1 a file failed, 64 usage.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)

dir=./models
layout=heron
tokenizer=0
verify=0
manifest=${FETCH_MODELS_MANIFEST:-$repo_root/models/MANIFEST}
patch_script=$script_dir/patch_figure_classifier.py

usage() {
  echo "Usage: $0 [--dir DIR] [--layout heron|picodet|all] [--tokenizer] [--verify] [--manifest FILE]" >&2
  exit 64
}

parse_args() {
  while [ $# -gt 0 ]; do
    case $1 in
      --dir) [ $# -ge 2 ] || usage; dir=$2; shift 2 ;;
      --layout) [ $# -ge 2 ] || usage; layout=$2; shift 2 ;;
      --manifest) [ $# -ge 2 ] || usage; manifest=$2; shift 2 ;;
      --tokenizer) tokenizer=1; shift ;;
      --verify) verify=1; shift ;;
      -h|--help) usage ;;
      *) echo "unknown argument: $1" >&2; usage ;;
    esac
  done
  case $layout in heron|picodet|all) ;; *) echo "--layout must be heron, picodet or all" >&2; usage ;; esac
  [ -r "$manifest" ] || { echo "manifest not readable: $manifest" >&2; exit 64; }
}

# The manifest as parallel arrays, comments and blank lines dropped.
m_path=() m_sha=() m_size=() m_group=() m_license=() m_url=()
load_manifest() {
  local path sha size group license url rest
  while read -r path sha size group license url rest; do
    case $path in ''|'#'*) continue ;; esac
    if [ -z "$url" ] || [ -n "$rest" ]; then
      echo "manifest line malformed (expect 6 columns): $path" >&2
      exit 64
    fi
    m_path+=("$path"); m_sha+=("$sha"); m_size+=("$size")
    m_group+=("$group"); m_license+=("$license"); m_url+=("$url")
  done <"$manifest"
  [ ${#m_path[@]} -gt 0 ] || { echo "manifest is empty: $manifest" >&2; exit 64; }
}

selected() {
  case $1 in
    ocr|table|figure-upstream|figure) return 0 ;;
    layout-heron) [ "$layout" = heron ] || [ "$layout" = all ] ;;
    layout-picodet) [ "$layout" = picodet ] || [ "$layout" = all ] ;;
    tokenizer) [ "$tokenizer" = 1 ] ;;
    *) echo "unknown manifest group: $1" >&2; exit 64 ;;
  esac
}

required() { [ "$1" = ocr ]; }

sha_of() { sha256sum "$1" | cut -c1-64; }

# 0 when the file exists and hashes to the manifest value.
matches() {
  [ -f "$1" ] && [ "$(sha_of "$1")" = "$2" ]
}

# Resolve the python prefix once; empty means "no way to run the patch".
python_cmd=
resolve_python() {
  if [ -n "${FETCH_MODELS_PYTHON:-}" ]; then
    python_cmd=$FETCH_MODELS_PYTHON
  elif command -v uv >/dev/null 2>&1; then
    python_cmd="uv run --with onnx --with onnxruntime --with numpy python"
  elif command -v python3 >/dev/null 2>&1; then
    if python3 -c 'import onnx' 2>/dev/null; then
      python_cmd=python3
    else
      echo "python3 cannot import the onnx module; install it (pip install onnx) or install uv" >&2
      return 1
    fi
  else
    echo "neither uv nor python3 found; the figure classifier patch needs one" >&2
    return 1
  fi
}

download() {
  local url=$1 target=$2 part="$2.part"
  mkdir -p "$(dirname "$target")"
  curl --fail --location --silent --show-error --retry 3 --retry-all-errors \
    --continue-at - --output "$part" "$url"
  mv "$part" "$target"
}

patch_from() {
  local source=$1 target=$2
  if ! matches "$source" "$3"; then
    echo "  patch source $source is absent or does not match the manifest" >&2
    return 1
  fi
  resolve_python || return 1
  # shellcheck disable=SC2086  # the prefix is a command line on purpose
  $python_cmd "$patch_script" "$source" "$target.part" >/dev/null
  mv "$target.part" "$target"
}

# Per-file outcome for the final table.
r_path=() r_status=() r_ok=()
record() { r_path+=("$1"); r_status+=("$2"); r_ok+=("$3"); }

failures=0
fail() { failures=$((failures + 1)); echo "  FAIL $1: $2" >&2; }

verify_one() {
  local i=$1 file="$dir/${m_path[$1]}"
  if matches "$file" "${m_sha[$i]}"; then
    record "${m_path[$i]}" present yes
  elif [ -f "$file" ]; then
    record "${m_path[$i]}" mismatch NO
    fail "${m_path[$i]}" "sha256 $(sha_of "$file") differs from the manifest"
  elif required "${m_group[$i]}"; then
    record "${m_path[$i]}" missing NO
    fail "${m_path[$i]}" "required file is missing"
  else
    record "${m_path[$i]}" absent -
  fi
}

# The sha of the source of a patch:<path> URL, looked up in the manifest.
sha_for_path() {
  local j
  for j in "${!m_path[@]}"; do
    if [ "${m_path[$j]}" = "$1" ]; then echo "${m_sha[$j]}"; return 0; fi
  done
  echo "patch source not in the manifest: $1" >&2
  return 64
}

fetch_one() {
  local i=$1 file="$dir/${m_path[$1]}" status=downloaded source_sha
  if matches "$file" "${m_sha[$i]}"; then
    record "${m_path[$i]}" present yes
    return 0
  fi
  echo "  ${m_path[$i]}: ${m_url[$i]}"
  case ${m_url[$i]} in
    patch:*)
      status=patched
      source_sha=$(sha_for_path "${m_url[$i]#patch:}") || exit $?
      patch_from "$dir/${m_url[$i]#patch:}" "$file" "$source_sha" || { record "${m_path[$i]}" "patch failed" NO; fail "${m_path[$i]}" "patch step failed"; return 0; }
      ;;
    *)
      download "${m_url[$i]}" "$file" || { record "${m_path[$i]}" "download failed" NO; fail "${m_path[$i]}" "download failed"; return 0; }
      ;;
  esac
  if matches "$file" "${m_sha[$i]}"; then
    record "${m_path[$i]}" "$status" yes
  else
    record "${m_path[$i]}" "$status" NO
    fail "${m_path[$i]}" "sha256 $(sha_of "$file") differs from the manifest; removed"
    rm -f "$file"
  fi
}

print_table() {
  local i
  printf '\n%-36s %-16s %s\n' file status 'sha ok'
  for i in "${!r_path[@]}"; do
    printf '%-36s %-16s %s\n' "${r_path[$i]}" "${r_status[$i]}" "${r_ok[$i]}"
  done
}

main() {
  parse_args "$@"
  load_manifest
  local i
  if [ "$verify" = 1 ]; then
    echo "verifying $dir against $manifest (layout=$layout tokenizer=$tokenizer)"
  else
    mkdir -p "$dir"
    echo "fetching into $dir (layout=$layout tokenizer=$tokenizer)"
  fi
  for i in "${!m_path[@]}"; do
    selected "${m_group[$i]}" || continue
    if [ "$verify" = 1 ]; then verify_one "$i"; else fetch_one "$i"; fi
  done
  print_table
  if [ "$failures" -gt 0 ]; then
    echo "$failures file(s) failed" >&2
    exit 1
  fi
  echo "all selected files verified"
}

main "$@"
