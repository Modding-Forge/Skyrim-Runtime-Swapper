#!/usr/bin/env bash
set -euo pipefail

case_filter=all
if [[ $# -eq 4 ]]; then
  case_filter="$4"
fi
if [[ $# -lt 3 || $# -gt 4 ||
      "$case_filter" != all && "$case_filter" != exfat &&
      "$case_filter" != ntfs3g ]]; then
  echo "usage: run-linux-persistent-filesystem-matrix.sh <native-sidecar> <baseline-game-root> <patch-root> [exfat|ntfs3g]" >&2
  exit 2
fi

sidecar="$(realpath "$1")"
baseline="$(realpath "$2")"
patches="$(realpath "$3")"
script_root="$(dirname "$(realpath "$0")")"
invoke="$script_root/invoke-sidecar-operation.py"
if [[ ! -x "$sidecar" || ! -d "$baseline" || ! -d "$patches" ||
      ! -f "$invoke" ]]; then
  echo "the sidecar, baseline, patch directory, or invocation helper is unavailable" >&2
  exit 2
fi
if [[ ${EUID} -ne 0 ]]; then
  echo "the persistent filesystem matrix must run as root" >&2
  exit 2
fi
for command in cmp cp cut du find flock losetup mkfs.ext4 mkfs.exfat mkfs.ntfs \
    mount mountpoint ntfs-3g python3 realpath sync truncate umount; do
  command -v "$command" >/dev/null || {
    echo "missing test dependency: $command" >&2
    exit 2
  }
done

test_root="$(mktemp -d -p "${TMPDIR:-/tmp}" srs-persistent-filesystems.XXXXXX)"
if [[ "$test_root" != /tmp/srs-persistent-filesystems.* &&
      "$test_root" != "${TMPDIR:-/tmp}"/srs-persistent-filesystems.* ]]; then
  echo "refusing unexpected test root: $test_root" >&2
  exit 2
fi
mounts=()
loops=()
cleanup() {
  local index
  for ((index=${#mounts[@]}-1; index>=0; --index)); do
    mountpoint -q "${mounts[index]}" && umount -l "${mounts[index]}" || true
  done
  for ((index=${#loops[@]}-1; index>=0; --index)); do
    losetup -d "${loops[index]}" 2>/dev/null || true
  done
  rm -rf -- "$test_root"
}
trap cleanup EXIT

attach_loop() {
  local image="$1"
  attached_loop="$(losetup --find --show "$image")"
  loops+=("$attached_loop")
}

mount_loop() {
  local filesystem="$1"
  local loop="$2"
  local target="$3"
  mkdir -p "$target"
  if [[ "$filesystem" == ntfs3g ]]; then
    mount -t ntfs-3g "$loop" "$target"
  else
    mount "$loop" "$target"
  fi
  mounts+=("$target")
}

unmount_path() {
  local target="$1"
  umount "$target"
  local remaining=()
  local item
  for item in "${mounts[@]}"; do
    [[ "$item" != "$target" ]] && remaining+=("$item")
  done
  mounts=("${remaining[@]}")
}

verify_baseline() {
  local game="$1"
  while IFS= read -r -d '' source; do
    local relative="${source#"$baseline"/}"
    [[ -f "$game/$relative" ]] || {
      echo "persistent restore omitted baseline file: $relative" >&2
      return 1
    }
    cmp -s "$source" "$game/$relative" || {
      echo "persistent restore changed baseline file: $relative" >&2
      return 1
    }
  done < <(find "$baseline" -type f -print0)
}

baseline_bytes="$(du -sb --apparent-size "$baseline" | cut -f1)"
patch_bytes="$(du -sb --apparent-size "$patches" | cut -f1)"
target_mib=$(( (baseline_bytes + patch_bytes + 1073741823) / 1048576 + 2048 ))
(( target_mib < 3072 )) && target_mib=3072
vault_mib=$(( (baseline_bytes * 3 + 1073741823) / 1048576 + 2048 ))
(( vault_mib < 4096 )) && vault_mib=4096

vault_image="$test_root/vault.ext4"
truncate -s "${vault_mib}M" "$vault_image"
mkfs.ext4 -q -F "$vault_image"
attach_loop "$vault_image"
vault_loop="$attached_loop"
vault_mount="$test_root/vault"
mount_loop ext4 "$vault_loop" "$vault_mount"

run_case() {
  local filesystem="$1"
  local expected_mode="$2"
  local filesystem_lock
  if [[ "$filesystem" == ntfs3g ]]; then
    exec {filesystem_lock}>/run/lock/srs-ntfs3g-persistent-matrix.lock
    flock "$filesystem_lock"
  fi
  local risk_flag=()
  [[ "$filesystem" == ntfs3g ]] && risk_flag=(--risk-accepted)
  local image="$test_root/target-$filesystem.img"
  truncate -s "${target_mib}M" "$image"
  attach_loop "$image"
  local loop="$attached_loop"
  if [[ "$filesystem" == exfat ]]; then
    mkfs.exfat "$loop" >/dev/null
  else
    mkfs.ntfs -F -Q "$loop" >/dev/null
  fi
  local target="$test_root/target-$filesystem"
  mount_loop "$filesystem" "$loop" "$target"
  local game="$target/game"
  mkdir -p "$game/RuntimeSwap/patches"
  cp -R --no-preserve=mode,ownership,timestamps -- "$baseline"/. "$game"/
  cp -R --no-preserve=mode,ownership,timestamps -- \
    "$patches"/. "$game/RuntimeSwap/patches"/

  local state="$vault_mount/state-$filesystem"
  local catalog="$vault_mount/catalog-$filesystem/ContentCatalog.txt"
  mkdir -m 0700 "$state"
  mkdir -p "$(dirname "$catalog")" "$test_root/home"
  local environment=(env XDG_STATE_HOME="$state" HOME="$test_root/home")
  "${environment[@]}" python3 "$invoke" "$sidecar" probe \
    "$game" "$catalog" --expect-mode "$expected_mode" --expect-not-persistent \
    "${risk_flag[@]}" >/dev/null
  "${environment[@]}" python3 "$invoke" "$sidecar" activate_persistent \
    "$game" "$catalog" --expect-mode "$expected_mode" --expect-persistent \
    "${risk_flag[@]}" >/dev/null
  sync -f "$game"
  unmount_path "$target"
  mount_loop "$filesystem" "$loop" "$target"

  "${environment[@]}" python3 "$invoke" "$sidecar" recover \
    "$game" "$catalog" --expect-mode "$expected_mode" --expect-persistent \
    "${risk_flag[@]}" >/dev/null
  "${environment[@]}" python3 "$invoke" "$sidecar" activate_persistent \
    "$game" "$catalog" --expect-mode "$expected_mode" --expect-persistent \
    "${risk_flag[@]}" >/dev/null
  "${environment[@]}" python3 "$invoke" "$sidecar" restore_persistent \
    "$game" "$catalog" --expect-mode "$expected_mode" --expect-not-persistent \
    "${risk_flag[@]}" >/dev/null
  verify_baseline "$game"
  sync -f "$game"
  unmount_path "$target"
  mount_loop "$filesystem" "$loop" "$target"
  "${environment[@]}" python3 "$invoke" "$sidecar" recover \
    "$game" "$catalog" --expect-mode "$expected_mode" --expect-not-persistent \
    "${risk_flag[@]}" >/dev/null
  verify_baseline "$game"
  unmount_path "$target"
  losetup -d "$loop"
  local remaining=()
  local item
  for item in "${loops[@]}"; do
    [[ "$item" != "$loop" ]] && remaining+=("$item")
  done
  loops=("${remaining[@]}")
  if [[ -n ${filesystem_lock:-} ]]; then
    exec {filesystem_lock}>&-
  fi
  echo "$filesystem persistent activation, remount, idempotence, and restore passed"
}

if [[ "$case_filter" == all || "$case_filter" == exfat ]]; then
  run_case exfat persistent_only
fi
if [[ "$case_filter" == all || "$case_filter" == ntfs3g ]]; then
  run_case ntfs3g persistent_with_warning
fi

echo "Linux persistent filesystem matrix passed"
