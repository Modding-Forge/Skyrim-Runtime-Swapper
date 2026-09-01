#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <mountpoint> <state-file>" >&2
  exit 2
fi

mountpoint=$1
state_file=$2

if mountpoint -q "$mountpoint"; then
  umount "$mountpoint"
fi
if [[ -f "$state_file" ]]; then
  loop_device=$(<"$state_file")
  if [[ "$loop_device" == /dev/loop* ]]; then
    losetup --detach "$loop_device" 2>/dev/null || true
  fi
  rm -f -- "$state_file"
fi
rmdir -- "$mountpoint" 2>/dev/null || true
