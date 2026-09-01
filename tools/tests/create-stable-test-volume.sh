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
printf '%s\n\n0\n' "$loop_device" >"$state_file"
mkfs.ext4 -q -F "$loop_device"
filesystem_uuid=$(blkid -s UUID -o value "$loop_device")
uuid_link="/dev/disk/by-uuid/$filesystem_uuid"
mkdir -p /dev/disk/by-uuid
if [[ -L "$uuid_link" && "$(readlink -f "$uuid_link")" == "$loop_device" ]]; then
  created_uuid_link=0
elif ln -s "$loop_device" "$uuid_link" 2>/dev/null; then
  created_uuid_link=1
elif [[ -L "$uuid_link" && "$(readlink -f "$uuid_link")" == "$loop_device" ]]; then
  created_uuid_link=0
else
  echo "refusing to replace existing filesystem UUID entry: $uuid_link" >&2
  exit 1
fi
printf '%s\n%s\n%s\n' "$loop_device" "$uuid_link" "$created_uuid_link" \
  >"$state_file"
mkdir -p "$mountpoint"
mount -o nosuid,nodev "$loop_device" "$mountpoint"
chown "$owner" "$mountpoint"
install -d -m 0700 -o "$owner" "$mountpoint/tests"
