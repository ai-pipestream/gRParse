# shellcheck shell=bash
# Sourced by the storage connector, remote source and chart end to end
# scripts. run_e2e_binary runs one test binary with the given environment:
# from a build tree when the script was given a directory, else inside the
# image it was given (Dockerfile.cpu --target e2e, which carries the
# binaries on the runtime base). The container shares the host network, so
# the emulators the script published on 127.0.0.1 answer at the same
# addresses, and mounts the script's work directory at the same path, as the
# user that owns it. The script sets every variable the binary needs, so an
# exit 77 (the binary's skip) is a broken run and fails here: a skip never
# counts as a pass.
#
#   run_e2e_binary <build-dir|image> <work-dir> <binary> [NAME=value...]
run_e2e_binary() {
  local from=$1 work=$2 binary=$3 status=0
  shift 3
  if [ -d "$from" ]; then
    env "$@" "$from/$binary" || status=$?
  else
    local flags=()
    for assignment in "$@"; do flags+=(-e "$assignment"); done
    docker run --rm --network host --user "$(id -u):$(id -g)" -v "$work:$work" \
      "${flags[@]}" --entrypoint "/usr/local/bin/$binary" "$from" || status=$?
  fi
  if [ "$status" -eq 77 ]; then
    echo "$binary skipped, which an end to end run counts as a failure" >&2
    return 1
  fi
  return "$status"
}
