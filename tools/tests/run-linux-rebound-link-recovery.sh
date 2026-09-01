#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run-linux-rebound-link-recovery.sh <runtime-probe> <baseline> <patch-root>" >&2
  exit 2
fi

probe="$(realpath "$1")"
baseline="$(realpath "$2")"
patches="$(realpath "$3")"
test_base="${SRS_TEST_ROOT_BASE:-${XDG_STATE_HOME:-$HOME/.local/state}}"
mkdir -p -m 0700 "$test_base"
test_base="$(realpath "$test_base")"
test_root="$(mktemp -d -p "$test_base" srs-rebound-links.XXXXXX)"
if [[ "$test_root" != "$test_base"/srs-rebound-links.* ]]; then
  echo "refusing unexpected test root: $test_root" >&2
  exit 2
fi
cleanup() {
  local status=$?
  if [[ ${data_core_mounted:-0} -eq 1 ]]; then
    umount -- "$game/Data_Core" || status=1
  fi
  if [[ $status -ne 0 && ${SRS_KEEP_FAILED_TEST_ROOT:-0} == 1 ]]; then
    echo "Retained failed test root: $test_root" >&2
    return
  fi
  rm -rf -- "$test_root"
}
trap cleanup EXIT

umask 022
library="$test_root/SteamLibrary"
game="$library/steamapps/common/Skyrim Special Edition"
# Amethyst's symlink deploy moves vanilla Data files into the sibling
# Data_Core tree and exposes them through final file links in Data.
deployment="$game/Data_Core"
deployment_store="$deployment"
patch_deployment="$test_root/Amethyst/Root_Folder/RuntimeSwap/patches"
state="$test_root/state"
data_core_mounted=0
if [[ ${SRS_TEST_BIND_DATA_CORE:-0} == 1 ]]; then
  deployment_store="$game/.amethyst-backing/Data_Core"
  mkdir -p "$deployment" "$deployment_store"
  mount --bind "$deployment_store" "$deployment"
  data_core_mounted=1
fi
mkdir -p "$game/RuntimeSwap/patches" "$deployment_store" "$patch_deployment" "$state"
chmod 0700 "$state"
cp -a -- "$baseline"/. "$game"/
cp -a -- "$patches"/. "$patch_deployment"/
# `sudo` keeps the copied DrvFS ownership on WSL; the product correctly
# requires managed files to belong to the executing user, so normalize only
# this disposable fixture to that user (root in privileged bind-mount runs).
chown -R "$(id -u):$(id -g)" "$game" "$patch_deployment"

# Amethyst deploys SRS's read-only patch assets as final links as well. The
# patch adapter must bind and verify their targets without ever writing through
# those links.
while IFS= read -r -d '' deployed_patch; do
  relative="${deployed_patch#"$patch_deployment"/}"
  logical_patch="$game/RuntimeSwap/patches/$relative"
  mkdir -p -- "$(dirname -- "$logical_patch")"
  ln -s -- "$deployed_patch" "$logical_patch"
done < <(find "$patch_deployment" -type f -print0)

# Amethyst can redeploy an RC10/RC11 transaction tree captured after a crash.
# In those builds staged/0 belonged to Dawnguard.esm and collided with the next
# patch output. RC12 must treat this only as legacy cleanup input and stage the
# new transaction in the Steam-library workspace.
mkdir -p "$game/.skyrim-runtime-swapper/transaction/staged"
printf 'stale-amethyst-overwrite' \
  >"$game/.skyrim-runtime-swapper/transaction/staged/0"

# A byte-identical launcher copy selects the renamed-SKSE alias layout. RC10's
# human-readable profile for this layout exceeded its on-disk field and was
# silently truncated.
cp --dereference -- "$game/SkyrimSELauncher.exe" "$game/skse64_loader.exe"

managed=(
  "SkyrimSE.exe"
  "SkyrimSELauncher.exe"
  "binkw64.dll"
  "steam_api64.dll"
  "Data/Skyrim - Shaders.bsa"
  "Data/Skyrim - Interface.bsa"
  "Data/Skyrim.esm"
  "Data/Update.esm"
  "Data/Dawnguard.esm"
  "Data/HearthFires.esm"
  "Data/Dragonborn.esm"
)
for relative in "${managed[@]}"; do
  logical="$game/$relative"
  [[ -f "$logical" ]] || continue
  if [[ "$relative" == Data/* ]]; then
    target="$deployment/${relative#Data/}"
    storage_target="$deployment_store/${relative#Data/}"
  else
    target="$game/.amethyst-root/$relative"
    storage_target="$target"
  fi
  mkdir -p -- "$(dirname -- "$storage_target")"
  mv -- "$logical" "$storage_target"
  ln -s -- "$target" "$logical"
done

if [[ ${SRS_TEST_BIND_DATA_CORE:-0} == 1 ]]; then
  # Keep the Data_Core links bound for both directions. The probe deliberately
  # damages one target file after activation, then restores it through the
  # reverse-patch/vault path on that same mount.
  XDG_STATE_HOME="$state" HOME="$test_root/home" \
    timeout 900 "$probe" "$game" "$game/RuntimeSwap/patches" >/dev/null
else
  XDG_STATE_HOME="$state" HOME="$test_root/home" \
    SRS_TEST_REBIND_MANAGED_LINKS=1 \
    timeout 900 "$probe" "$game" "$game/RuntimeSwap/patches" >/dev/null
fi

[[ ! -e "$game/.skyrim-runtime-swapper" ]]
[[ -d "$library/.runtime-swapper/work" ]]
if find "$game" -name '.*.srs-*' -print -quit | grep -q .; then
  echo "mount-local transaction files remained after recovery" >&2
  exit 1
fi

echo "Managed-link transaction recovery passed"
