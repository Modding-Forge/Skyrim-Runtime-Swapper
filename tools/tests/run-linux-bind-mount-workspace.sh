#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: run-linux-bind-mount-workspace.sh <posix-storage-tests>" >&2
  exit 2
fi

test_binary="$(realpath "$1")"
test_root="$(mktemp -d /tmp/srs-bind-workspace.XXXXXX)"
if [[ "$test_root" != /tmp/srs-bind-workspace.* ]]; then
  echo "refusing unexpected test root: $test_root" >&2
  exit 2
fi
mounted=0
cleanup() {
  local status=$?
  if [[ $mounted -eq 1 ]]; then
    umount -- "$test_root/game/Data_Core" || status=1
  fi
  rm -rf -- "$test_root"
  exit "$status"
}
trap cleanup EXIT

mkdir -p "$test_root/backing/Data_Core" \
         "$test_root/game/Data_Core" \
         "$test_root/SteamLibrary/.runtime-swapper/work/skyrimse-test"
mount --bind "$test_root/backing/Data_Core" "$test_root/game/Data_Core"
mounted=1

SRS_TEST_BIND_MOUNT_TARGET="$test_root/game/Data_Core" \
SRS_TEST_DEFAULT_WORK_ROOT="$test_root/SteamLibrary/.runtime-swapper/work/skyrimse-test/0123456789abcdef0123456789abcdef" \
  "$test_binary"

echo "Bind-mount transaction workspace passed"
