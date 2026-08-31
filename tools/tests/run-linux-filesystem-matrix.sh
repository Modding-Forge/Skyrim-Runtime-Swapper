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

for command in losetup mount umount truncate mkfs.ext2 mkfs.ext4 mkfs.xfs mkfs.btrfs \
    mkfs.exfat mkfs.vfat mkfs.ntfs ntfs-3g; do
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
    ext2) mkfs.ext2 -q -F "$loop" ;;
    ext4) mkfs.ext4 -q -F "$loop" ;;
    xfs) mkfs.xfs -q -f "$loop" ;;
    btrfs) mkfs.btrfs -q -f "$loop" ;;
    exfat) mkfs.exfat "$loop" >/dev/null ;;
    vfat) mkfs.vfat "$loop" >/dev/null ;;
    ntfs3g) mkfs.ntfs -F -Q "$loop" >/dev/null ;;
    *) echo "unsupported matrix filesystem: $filesystem" >&2; exit 2 ;;
  esac
  mkdir -p "$mount_path"
  if [[ "$filesystem" == ntfs3g ]]; then
    mount -t ntfs-3g "$loop" "$mount_path"
  else
    mount "$loop" "$mount_path"
  fi
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
  local actual
  make_volume "target-$filesystem" "$filesystem" 512
  target_mount="$created_mount"
  mkdir -p "$target_mount/game"
  output="$(XDG_STATE_HOME="$vault_state" HOME="$test_root/home" \
    "$probe" "$target_mount/game" --prepare)"
  printf '%s\n' "$output"
  actual="$(sed -n 's/^mode=//p' <<<"$output")"
  if [[ "$actual" != "$expected" ]]; then
    echo "$filesystem was classified as $actual instead of $expected" >&2
    return 1
  fi
  created="$(sed -n 's/^vault=//p' <<<"$output")"
  [[ "$created" == "$vault_state"/modding-forge/skyrim-runtime-swapper/vaults/skyrimse-* ]]
  [[ -d "$created" && ! -L "$created" && "$(stat -c '%a' "$created")" == 700 ]]
}

run_case ext4 automatic
run_case xfs automatic
run_case btrfs automatic
run_case exfat persistent_only
run_case vfat persistent_with_warning
run_case ext2 persistent_with_warning
run_case ntfs3g persistent_with_warning

original_output="$(XDG_STATE_HOME="$vault_state" HOME="$test_root/home" \
  "$probe" "${mounts[1]}/game")"
printf '%s\n' "$original_output"
[[ "$(sed -n 's/^mode=//p' <<<"$original_output")" == automatic ]]
original_id="$(sed -n 's/^installation=//p' <<<"$original_output")"
bind_mount="$test_root/bind-game"
mkdir -p "$bind_mount"
mount --bind "${mounts[1]}/game" "$bind_mount"
mounts+=("$bind_mount")
bind_output="$(XDG_STATE_HOME="$vault_state" HOME="$test_root/home" \
  "$probe" "$bind_mount")"
printf '%s\n' "$bind_output"
[[ "$(sed -n 's/^mode=//p' <<<"$bind_output")" == automatic ]]
bind_id="$(sed -n 's/^installation=//p' <<<"$bind_output")"
if [[ -z "$original_id" || "$bind_id" != "$original_id" ]]; then
  echo "installation identity changed through a bind mount" >&2
  exit 1
fi

make_volume same-exfat exfat 1024
same_mount="$created_mount"
mkdir -p "$same_mount/game" "$same_mount/state"
XDG_STATE_HOME="$same_mount/state" HOME="$test_root/home" \
  "$probe" "$same_mount/game" hard_blocked

first_target="${mounts[1]}/game"
make_volume undersized-vault ext4 128
undersized_mount="$created_mount"
mkdir -p "$undersized_mount/state"
undersized_output="$(XDG_STATE_HOME="$undersized_mount/state" \
  HOME="$test_root/home" "$probe" "$first_target" hard_blocked --prepare)"
printf '%s\n' "$undersized_output"
[[ "$(sed -n 's/^mode=//p' <<<"$undersized_output")" == hard_blocked ]]
[[ "$(sed -n 's/^technical_reason=//p' <<<"$undersized_output")" == \
   vault-insufficient-space ]]

make_volume read-only-vault ext4 512
read_only_mount="$created_mount"
mkdir -p "$read_only_mount/state"
mount -o remount,ro "$read_only_mount"
read_only_output="$(XDG_STATE_HOME="$read_only_mount/state" \
  HOME="$test_root/home" "$probe" "$first_target" hard_blocked --prepare)"
printf '%s\n' "$read_only_output"
[[ "$(sed -n 's/^mode=//p' <<<"$read_only_output")" == hard_blocked ]]
[[ "$(sed -n 's/^technical_reason=//p' <<<"$read_only_output")" == \
   vault-create-failed ]]

mkdir -p "$test_root/tmpfs" "$test_root/tmpfs/game"
mount -t tmpfs -o size=64m tmpfs "$test_root/tmpfs"
mounts+=("$test_root/tmpfs")
mkdir -p "$test_root/tmpfs/game"
XDG_STATE_HOME="$vault_state" HOME="$test_root/home" \
  "$probe" "$test_root/tmpfs/game" hard_blocked

ln -s "$vault_state" "$test_root/symlink-state"
XDG_STATE_HOME="$test_root/symlink-state" HOME="$test_root/home" \
  "$probe" "$first_target" hard_blocked

echo "Linux filesystem safety matrix passed"
