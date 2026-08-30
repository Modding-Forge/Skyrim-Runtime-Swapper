#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: run-linux-filesystem-matrix.sh <storage_backend_probe>" >&2
  exit 2
fi
probe="$(realpath "$1")"
if [[ ! -x "$probe" ]]; then
  echo "probe is not executable: $probe" >&2
  exit 2
fi
if [[ ${EUID} -ne 0 ]]; then
  echo "the isolated loop-device matrix must run as root" >&2
  exit 2
fi

for command in losetup mount umount truncate mkfs.ext4 mkfs.xfs mkfs.btrfs mkfs.exfat mkfs.vfat; do
  command -v "$command" >/dev/null || {
    echo "missing test dependency: $command" >&2
    exit 2
  }
done

test_root="$(mktemp -d -p "${TMPDIR:-/tmp}" srs-storage-matrix.XXXXXX)"
if [[ "$test_root" != /tmp/srs-storage-matrix.* &&
      "$test_root" != "${TMPDIR:-/tmp}"/srs-storage-matrix.* ]]; then
  echo "refusing unexpected test root: $test_root" >&2
  exit 2
fi
declare -a mounts=()
declare -a loops=()

cleanup() {
  set +e
  for ((index=${#mounts[@]}-1; index>=0; --index)); do
    mountpoint -q "${mounts[index]}" && umount "${mounts[index]}"
  done
  for ((index=${#loops[@]}-1; index>=0; --index)); do
    losetup -d "${loops[index]}"
  done
  rm -rf -- "$test_root"
}
trap cleanup EXIT

make_volume() {
  local name="$1"
  local filesystem="$2"
  local size_mib="$3"
  local image="$test_root/$name.img"
  local mount_path="$test_root/mnt-$name"
  truncate -s "${size_mib}M" "$image"
  local loop
  loop="$(losetup --find --show "$image")"
  loops+=("$loop")
  case "$filesystem" in
    ext4) mkfs.ext4 -q -F "$loop" ;;
    xfs) mkfs.xfs -q -f "$loop" ;;
    btrfs) mkfs.btrfs -q -f "$loop" ;;
    exfat) mkfs.exfat "$loop" >/dev/null ;;
    vfat) mkfs.vfat "$loop" >/dev/null ;;
    *) echo "unsupported matrix filesystem: $filesystem" >&2; exit 2 ;;
  esac
  mkdir -p "$mount_path"
  mount "$loop" "$mount_path"
  mounts+=("$mount_path")
  created_mount="$mount_path"
}

created_mount=""
make_volume vault ext4 1024
vault_mount="$created_mount"
vault_state="$vault_mount/state"
mkdir -p "$vault_state"
chmod 0700 "$vault_state"

run_case() {
  local filesystem="$1"
  local expected="$2"
  local target_mount
  local output
  local created
  make_volume "target-$filesystem" "$filesystem" 512
  target_mount="$created_mount"
  mkdir -p "$target_mount/game"
  output="$(XDG_STATE_HOME="$vault_state" HOME="$test_root/home" \
    "$probe" "$target_mount/game" "$expected" --prepare)"
  printf '%s\n' "$output"
  created="$(sed -n 's/^vault=//p' <<<"$output")"
  [[ "$created" == "$vault_state"/modding-forge/skyrim-runtime-swapper/vaults/skyrimse-* ]]
  [[ -d "$created" && ! -L "$created" && "$(stat -c '%a' "$created")" == 700 ]]
}

run_case ext4 automatic
run_case xfs automatic
run_case btrfs automatic
run_case exfat persistent_only
run_case vfat persistent_with_warning

original_output="$(XDG_STATE_HOME="$vault_state" HOME="$test_root/home" \
  "$probe" "${mounts[1]}/game" automatic)"
original_id="$(sed -n 's/^installation=//p' <<<"$original_output")"
bind_mount="$test_root/bind-game"
mkdir -p "$bind_mount"
mount --bind "${mounts[1]}/game" "$bind_mount"
mounts+=("$bind_mount")
bind_output="$(XDG_STATE_HOME="$vault_state" HOME="$test_root/home" \
  "$probe" "$bind_mount" automatic)"
bind_id="$(sed -n 's/^installation=//p' <<<"$bind_output")"
[[ -n "$original_id" && "$bind_id" == "$original_id" ]]

make_volume same-exfat exfat 1024
same_mount="$created_mount"
mkdir -p "$same_mount/game" "$same_mount/state"
XDG_STATE_HOME="$same_mount/state" HOME="$test_root/home" \
  "$probe" "$same_mount/game" hard_blocked

mkdir -p "$test_root/tmpfs" "$test_root/tmpfs/game"
mount -t tmpfs -o size=64m tmpfs "$test_root/tmpfs"
mounts+=("$test_root/tmpfs")
mkdir -p "$test_root/tmpfs/game"
XDG_STATE_HOME="$vault_state" HOME="$test_root/home" \
  "$probe" "$test_root/tmpfs/game" hard_blocked

ln -s "$vault_state" "$test_root/symlink-state"
first_target="${mounts[1]}/game"
XDG_STATE_HOME="$test_root/symlink-state" HOME="$test_root/home" \
  "$probe" "$first_target" hard_blocked

echo "Linux filesystem safety matrix passed"
