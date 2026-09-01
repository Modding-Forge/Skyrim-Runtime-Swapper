#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run-ephemeral-recovery-integration.sh <sidecar> <baseline> <patch-root>" >&2
  exit 2
fi

# Reproduce the default desktop umask that exposed RC10's lock/vault ordering
# bug on both Fedora and Bazzite.
umask 022

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

activation_json="$(python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" activate_session "$game" "$catalog" \
  --expect-mode automatic --expect-not-persistent)"
transaction_work="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["transaction_work"])' \
  "$activation_json")"
[[ -f "$transaction_work/vault.locator" ]]
[[ ! -e "$game/.skyrim-runtime-swapper" ]]
[[ "$(stat -c %a "$library/.runtime-swapper")" == "700" ]]
[[ "$(stat -c %a "$library/.runtime-swapper/locks")" == "700" ]]
while IFS= read -r private_directory; do
  [[ "$(stat -c %a "$private_directory")" == "700" ]]
done < <(find "$library/.runtime-swapper/recovery" -maxdepth 2 -type d)
python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" recover "$game" "$catalog" \
  --expect-mode automatic --expect-not-persistent >/dev/null

[[ ! -e "$library/.runtime-swapper/recovery" ]]
[[ ! -e "$transaction_work" ]]
[[ ! -e "$game/.skyrim-runtime-swapper" ]]
[[ -d "$library/.runtime-swapper/cache" ]]
lock_count="$(find "$library/.runtime-swapper/locks" -maxdepth 1 -type f \
  -name 'skyrimse-*.lock' -printf . | wc -c)"
[[ "$lock_count" -eq 1 ]]

# A power loss can leave a disposable cache object with the expected size but
# invalid contents. It must be removed together with its staged candidate so
# the verified patch fallback can rebuild it during the same launch.
cache_object="$(find "$library/.runtime-swapper/cache" -type f -print -quit)"
[[ -n "$cache_object" ]]
python3 - "$cache_object" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
with path.open("r+b") as stream:
    first = stream.read(1)
    if not first:
        raise SystemExit("cache object is unexpectedly empty")
    stream.seek(0)
    stream.write(bytes([first[0] ^ 0xFF]))
    stream.flush()
PY
python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" activate_session "$game" "$catalog" \
  --expect-mode automatic --expect-not-persistent >/dev/null
cache_hash="$(basename -- "$cache_object")"
[[ "$(sha256sum "$cache_object" | cut -d' ' -f1)" == "$cache_hash" ]]
python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" recover "$game" "$catalog" \
  --expect-mode automatic --expect-not-persistent >/dev/null

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

persistent_json="$(python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" activate_persistent "$game" "$catalog" \
  --expect-mode automatic --expect-persistent)"
persistent_work="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["transaction_work"])' \
  "$persistent_json")"
[[ -d "$library/.runtime-swapper/recovery" ]]
[[ -f "$persistent_work/persistent.v2" ]]
[[ ! -e "$game/.skyrim-runtime-swapper" ]]
python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" restore_persistent "$game" "$catalog" \
  --expect-mode automatic --expect-not-persistent >/dev/null
[[ ! -e "$library/.runtime-swapper/recovery" ]]
[[ ! -e "$persistent_work" ]]
[[ ! -e "$game/.skyrim-runtime-swapper" ]]

# A missing vault must never be retired while the target runtime is active.
missing_vault_json="$(python3 "$script_root/invoke-sidecar-operation.py" \
  "$sidecar" activate_session "$game" "$catalog" \
  --expect-mode automatic --expect-not-persistent)"
missing_vault_work="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["transaction_work"])' \
  "$missing_vault_json")"
[[ -f "$missing_vault_work/vault.locator" ]]
rm -rf -- "$library/.runtime-swapper/recovery"
if python3 "$script_root/invoke-sidecar-operation.py" \
    "$sidecar" probe "$game" "$catalog" >/dev/null 2>&1; then
  echo "an orphaned locator was accepted while the target runtime was active" >&2
  exit 1
fi
[[ -f "$missing_vault_work/vault.locator" ]]

echo "Ephemeral recovery lifecycle integration passed"
