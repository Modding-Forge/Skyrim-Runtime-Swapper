#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <mountpoint> <state-file> <image>" >&2
  exit 2
fi

mountpoint=$1
state_file=$2
image=$3

if mountpoint -q "$mountpoint"; then
  umount "$mountpoint"
fi
if [[ -f "$state_file" ]]; then
  mapfile -t state <"$state_file"
  loop_device=${state[0]:-}
  uuid_link=${state[1]:-}
  created_uuid_link=${state[2]:-0}
  if [[ "$created_uuid_link" == 1 &&
        "$uuid_link" == /dev/disk/by-uuid/* && -L "$uuid_link" &&
        "$(readlink -f "$uuid_link")" == "$loop_device" ]]; then
    rm -f -- "$uuid_link"
  fi
  if [[ "$loop_device" == /dev/loop* ]]; then
    losetup --detach "$loop_device" 2>/dev/null || true
  fi
  rm -f -- "$state_file"
fi
rmdir -- "$mountpoint" 2>/dev/null || true
rm -f -- "$image"
