#!/usr/bin/env bash
set -euo pipefail

mode=all
if [[ $# -eq 4 ]]; then
  case "$1" in
    --flakey-only) mode=flakey ;;
    --corruption-only) mode=corruption ;;
    *) mode=invalid ;;
  esac
  shift
fi
if [[ $# -ne 3 ]]; then
  echo "usage: run-linux-block-fault-matrix.sh [--flakey-only|--corruption-only] <debug-runtime-probe> <baseline-game-root> <patch-root>" >&2
  exit 2
fi
if [[ "$mode" == invalid ]]; then
  echo "unknown block-fault matrix mode" >&2
  exit 2
fi
probe="$(realpath "$1")"
baseline="$(realpath "$2")"
patches="$(realpath "$3")"
if [[ ! -x "$probe" || ! -d "$baseline" || ! -d "$patches" ]]; then
  echo "the probe, baseline, or patch directory is unavailable" >&2
  exit 2
fi
if [[ ${EUID} -ne 0 ]]; then
  echo "the isolated device-mapper matrix must run as root" >&2
  exit 2
fi
commands=(blockdev cmp cp dmsetup e2fsck losetup mkfs.ext4 mount mountpoint
          realpath sync timeout truncate umount)
if [[ "$mode" == all ]]; then commands+=(replay-log); fi
for command in "${commands[@]}"; do
  command -v "$command" >/dev/null || {
    echo "missing test dependency: $command" >&2
    exit 2
  }
done
if ! dmsetup targets | grep -q '^flakey '; then
  modprobe dm_flakey 2>/dev/null || true
fi
if ! dmsetup targets | grep -q '^flakey '; then
  echo "the kernel does not provide the dm-flakey target" >&2
  exit 2
fi
if [[ "$mode" == all ]] && ! dmsetup targets | grep -q '^log-writes '; then
  modprobe dm_log_writes 2>/dev/null || true
  if ! dmsetup targets | grep -q '^log-writes '; then
    echo "the kernel does not provide the dm-log-writes target" >&2
    exit 2
  fi
fi

test_root="$(mktemp -d -p "${TMPDIR:-/tmp}" srs-block-faults.XXXXXX)"
if [[ "$test_root" != /tmp/srs-block-faults.* &&
      "$test_root" != "${TMPDIR:-/tmp}"/srs-block-faults.* ]]; then
  echo "refusing unexpected test root: $test_root" >&2
  exit 2
fi
declare -a mounts=()
declare -a loops=()
declare -a maps=()

cleanup() {
  set +e
  for ((index=${#mounts[@]}-1; index>=0; --index)); do
    mountpoint -q "${mounts[index]}" && umount -l "${mounts[index]}"
  done
  for ((index=${#maps[@]}-1; index>=0; --index)); do
    dmsetup remove --retry --force "${maps[index]}" 2>/dev/null
  done
  for ((index=${#loops[@]}-1; index>=0; --index)); do
    losetup -d "${loops[index]}" 2>/dev/null
  done
  rm -rf -- "$test_root"
}
trap cleanup EXIT

attach_loop() {
  attached_loop="$(losetup --find --show "$1")"
  loops+=("$attached_loop")
}

detach_loop() {
  local device="$1"
  losetup -d "$device"
  local remaining=()
  local item
  for item in "${loops[@]}"; do
    [[ "$item" != "$device" ]] && remaining+=("$item")
  done
  loops=("${remaining[@]}")
}

remove_map() {
  local name="$1"
  dmsetup remove --retry "$name"
  local remaining=()
  local item
  for item in "${maps[@]}"; do
    [[ "$item" != "$name" ]] && remaining+=("$item")
  done
  maps=("${remaining[@]}")
}

mount_device() {
  local device="$1"
  local path="$2"
  mkdir -p "$path"
  mount -o noatime "$device" "$path"
  mounts+=("$path")
}

unmount_device() {
  local path="$1"
  umount "$path"
  forget_mount "$path"
}

forget_mount() {
  local path="$1"
  local remaining=()
  local item
  for item in "${mounts[@]}"; do
    [[ "$item" != "$path" ]] && remaining+=("$item")
  done
  mounts=("${remaining[@]}")
}

verify_baseline() {
  local game="$1"
  while IFS= read -r -d '' source; do
    local relative="${source#"$baseline"/}"
    [[ -f "$game/$relative" ]] || {
      echo "recovery omitted baseline file: $relative" >&2
      return 1
    }
    cmp -s "$source" "$game/$relative" || {
      echo "recovery changed baseline file: $relative" >&2
      return 1
    }
  done < <(find "$baseline" -type f -print0)
}

repair_ext4() {
  local device="$1"
  local result
  set +e
  e2fsck -fy "$device" >/dev/null
  result=$?
  set -e
  if (( result > 1 )); then
    echo "e2fsck could not repair the injected filesystem damage (exit $result)" >&2
    return 1
  fi
}

baseline_bytes="$(du -sb --apparent-size "$baseline" | cut -f1)"
target_mib=$(( (baseline_bytes * 4 + 1073741823) / 1048576 + 1024 ))
(( target_mib < 2048 )) && target_mib=2048
vault_mib=$(( (baseline_bytes * 3 + 1073741823) / 1048576 + 1024 ))
(( vault_mib < 2048 )) && vault_mib=2048
log_mib=$(( target_mib * 2 ))

base_image="$test_root/baseline.ext4"
truncate -s "${target_mib}M" "$base_image"
mkfs.ext4 -q -F "$base_image"
attach_loop "$base_image"
base_loop="$attached_loop"
base_mount="$test_root/base"
mount_device "$base_loop" "$base_mount"
mkdir -p "$base_mount/game"
cp -a -- "$baseline"/. "$base_mount/game/"
sync -f "$base_mount/game"
unmount_device "$base_mount"
detach_loop "$base_loop"

vault_image="$test_root/vault.ext4"
truncate -s "${vault_mib}M" "$vault_image"
mkfs.ext4 -q -F "$vault_image"
attach_loop "$vault_image"
vault_loop="$attached_loop"
vault_mount="$test_root/vault"
mount_device "$vault_loop" "$vault_mount"

if [[ "$mode" == all ]]; then
for phase in {1..10}; do
  target_image="$test_root/log-target-$phase.ext4"
  replay_image="$test_root/log-replay-$phase.ext4"
  log_image="$test_root/log-$phase.bin"
  cp --reflink=auto --sparse=always "$base_image" "$target_image"
  truncate -s "${log_mib}M" "$log_image"
  attach_loop "$target_image"
  target_loop="$attached_loop"
  attach_loop "$log_image"
  log_loop="$attached_loop"
  sectors="$(blockdev --getsz "$target_loop")"
  map_name="srs-log-$phase-$$"
  dmsetup create "$map_name" --table \
    "0 $sectors log-writes $target_loop $log_loop"
  maps+=("$map_name")
  dmsetup message "$map_name" 0 mark baseline
  target_mount="$test_root/log-mount-$phase"
  mount_device "/dev/mapper/$map_name" "$target_mount"
  state="$vault_mount/state-log-$phase"
  mkdir -m 0700 "$state"

  set +e
  XDG_STATE_HOME="$state" HOME="$test_root/home" \
    SKYRIM_RUNTIME_SWAPPER_FAULT_AFTER_PHASE="$phase" \
    timeout 300 "$probe" "$target_mount/game" "$patches" >/dev/null 2>&1
  result=$?
  set -e
  if [[ $result -ne 201 ]]; then
    echo "journal phase $phase did not terminate the Debug probe (exit $result)" >&2
    exit 1
  fi

  # This deliberately avoids a filesystem flush: the persisted log is the
  # power-loss image at the last completed FUA/flush boundary.
  dmsetup suspend --noflush "$map_name"
  umount -l "$target_mount"
  forget_mount "$target_mount"
  remove_map "$map_name"
  detach_loop "$target_loop"
  detach_loop "$log_loop"

  cp --reflink=auto --sparse=always "$base_image" "$replay_image"
  attach_loop "$replay_image"
  replay_loop="$attached_loop"
  attach_loop "$log_image"
  log_loop="$attached_loop"
  replay-log --log "$log_loop" --replay "$replay_loop"
  repair_ext4 "$replay_loop"
  replay_mount="$test_root/replay-mount-$phase"
  mount_device "$replay_loop" "$replay_mount"
  XDG_STATE_HOME="$state" HOME="$test_root/home" \
    timeout 300 "$probe" "$replay_mount/game" "$patches" >/dev/null
  verify_baseline "$replay_mount/game"
  unmount_device "$replay_mount"
  detach_loop "$replay_loop"
  detach_loop "$log_loop"
done
fi

feature_specs=('error_writes|1|error_writes'
               'drop_writes|1|drop_writes'
               'corrupt_bio_byte|5|corrupt_bio_byte 32 w 1 0')
if [[ "$mode" == corruption ]]; then
  feature_specs=('corrupt_bio_byte|5|corrupt_bio_byte 32 w 1 0')
fi
for feature_spec in "${feature_specs[@]}"; do
  IFS='|' read -r slug feature_count feature <<<"$feature_spec"
  target_image="$test_root/flakey-$slug.ext4"
  cp --reflink=auto --sparse=always "$base_image" "$target_image"
  attach_loop "$target_image"
  target_loop="$attached_loop"
  sectors="$(blockdev --getsz "$target_loop")"
  map_name="srs-flakey-$slug-$$"
  dmsetup create "$map_name" --table \
    "0 $sectors flakey $target_loop 0 1 3600 $feature_count $feature"
  maps+=("$map_name")
  target_mount="$test_root/flakey-mount-$slug"
  mount_device "/dev/mapper/$map_name" "$target_mount"
  state="$vault_mount/state-flakey-$slug"
  mkdir -m 0700 "$state"
  sleep 2

  set +e
  XDG_STATE_HOME="$state" HOME="$test_root/home" \
    timeout 300 "$probe" "$target_mount/game" "$patches" >/dev/null 2>&1
  set -e
  umount -l "$target_mount"
  forget_mount "$target_mount"
  remove_map "$map_name"
  repair_ext4 "$target_loop"
  recovery_mount="$test_root/flakey-recovery-$slug"
  mount_device "$target_loop" "$recovery_mount"
  XDG_STATE_HOME="$state" HOME="$test_root/home" \
    timeout 300 "$probe" "$recovery_mount/game" "$patches" >/dev/null
  verify_baseline "$recovery_mount/game"
  unmount_device "$recovery_mount"
  detach_loop "$target_loop"
done

if [[ "$mode" == all ]]; then
  echo "Linux dm-log-writes and dm-flakey recovery matrix passed"
elif [[ "$mode" == corruption ]]; then
  echo "Linux dm-flakey corruption recovery passed"
else
  echo "Linux dm-flakey recovery matrix passed"
fi
