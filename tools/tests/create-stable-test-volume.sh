#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $0 <image> <mountpoint> <state-file> <owner>" >&2
  exit 2
fi

image=$1
mountpoint=$2
state_file=$3
owner=$4

truncate -s 8G "$image"
loop_device=$(losetup --find --show "$image")
printf '%s\n' "$loop_device" >"$state_file"
mkfs.ext4 -q -F "$loop_device"
mkdir -p "$mountpoint"
mount -o nosuid,nodev "$loop_device" "$mountpoint"
chown "$owner" "$mountpoint"
install -d -m 0700 -o "$owner" "$mountpoint/tests"
