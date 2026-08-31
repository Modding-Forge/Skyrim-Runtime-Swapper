#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run-ephemeral-recovery-integration.sh <sidecar> <baseline> <patch-root>" >&2
  exit 2
fi

sidecar="$(realpath "$1")"
baseline="$(realpath "$2")"
patches="$(realpath "$3")"
script_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
test_base="${SRS_TEST_ROOT_BASE:-${XDG_STATE_HOME:-$HOME/.local/state}}"
mkdir -p -m 0700 "$test_base"
test_base="$(realpath "$test_base")"
test_root="$(mktemp -d -p "$test_base" srs-ephemeral-recovery.XXXXXX)"
if [[ "$test_root" != "$test_base"/srs-ephemeral-recovery.* ]]; then
  echo "refusing unexpected test root: $test_root" >&2
  exit 2
fi
trap 'rm -rf -- "$test_root"' EXIT

library="$test_root/SteamLibrary"
game="$library/steamapps/common/Skyrim Special Edition"
catalog="$test_root/catalog/ContentCatalog.txt"
mkdir -p "$game/RuntimeSwap/patches" "$(dirname -- "$catalog")"
cp -a -- "$baseline"/. "$game"/
cp -a -- "$patches"/. "$game/RuntimeSwap/patches"/

python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" activate_session "$game" "$catalog" \
  --expect-mode automatic --expect-not-persistent >/dev/null
python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" recover "$game" "$catalog" \
  --expect-mode automatic --expect-not-persistent >/dev/null

[[ ! -e "$library/.runtime-swapper/recovery" ]]
[[ ! -e "$game/.skyrim-runtime-swapper" ]]
[[ -d "$library/.runtime-swapper/cache" ]]
lock_count="$(find "$library/.runtime-swapper/locks" -maxdepth 1 -type f \
  -name 'skyrimse-*.lock' -printf . | wc -c)"
[[ "$lock_count" -eq 1 ]]

probe_json="$(python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" probe "$game" "$catalog" \
  --expect-mode automatic --expect-not-persistent)"
installation="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["installation"])' \
  "$probe_json")"
mkdir -p -m 0700 "$game/.skyrim-runtime-swapper"
printf 'SRS-VAULT-LOCATOR-1\ninstallation=%s\nvault=%s\nvolume=missing-volume\n' \
  "$installation" "$test_root/missing-legacy-vault" \
  >"$game/.skyrim-runtime-swapper/vault.locator"
chmod 0600 "$game/.skyrim-runtime-swapper/vault.locator"
mkdir -p "$(dirname -- "$catalog")/.skyrim-runtime-swapper"
printf 'pending' >"$(dirname -- "$catalog")/.skyrim-runtime-swapper/ContentCatalog.hold"
if python3 "$script_root/invoke-sidecar-operation.py" \
    "$sidecar" probe "$game" "$catalog" >/dev/null 2>&1; then
  echo "an orphaned locator was accepted with pending ContentCatalog data" >&2
  exit 1
fi
[[ -f "$game/.skyrim-runtime-swapper/vault.locator" ]]
rm -- "$(dirname -- "$catalog")/.skyrim-runtime-swapper/ContentCatalog.hold"
printf 'preserve-me' >"$game/.skyrim-runtime-swapper/unknown-user-file"
if python3 "$script_root/invoke-sidecar-operation.py" \
    "$sidecar" probe "$game" "$catalog" >/dev/null 2>&1; then
  echo "an orphaned locator was accepted beside unknown transaction data" >&2
  exit 1
fi
[[ -f "$game/.skyrim-runtime-swapper/vault.locator" ]]
rm -- "$game/.skyrim-runtime-swapper/unknown-user-file"
python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" probe "$game" "$catalog" \
  --expect-mode automatic --expect-not-persistent >/dev/null
[[ ! -e "$game/.skyrim-runtime-swapper" ]]

python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" activate_persistent "$game" "$catalog" \
  --expect-mode automatic --expect-persistent >/dev/null
[[ -d "$library/.runtime-swapper/recovery" ]]
[[ -f "$game/.skyrim-runtime-swapper/persistent.v2" ]]
python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" restore_persistent "$game" "$catalog" \
  --expect-mode automatic --expect-not-persistent >/dev/null
[[ ! -e "$library/.runtime-swapper/recovery" ]]
[[ ! -e "$game/.skyrim-runtime-swapper" ]]

# A missing vault must never be retired while the target runtime is active.
python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" activate_session "$game" "$catalog" \
  --expect-mode automatic --expect-not-persistent >/dev/null
[[ -f "$game/.skyrim-runtime-swapper/vault.locator" ]]
rm -rf -- "$library/.runtime-swapper/recovery"
if python3 "$script_root/invoke-sidecar-operation.py" \
    "$sidecar" probe "$game" "$catalog" >/dev/null 2>&1; then
  echo "an orphaned locator was accepted while the target runtime was active" >&2
  exit 1
fi
[[ -f "$game/.skyrim-runtime-swapper/vault.locator" ]]

echo "Ephemeral recovery lifecycle integration passed"
