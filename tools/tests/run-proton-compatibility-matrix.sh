#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: run-proton-compatibility-matrix.sh <wine-bridge-directory>" >&2
  echo "set PROTON_STABLE, PROTON_EXPERIMENTAL, and PROTON_GE to their proton launchers" >&2
  exit 2
fi
bridge="$(realpath "$1")"
probe="$bridge/WineSidecarProbe.exe"
sidecar="$bridge/SkyrimRuntimeSwapper.Native"
if [[ ! -f "$probe" || ! -f "$sidecar" ]]; then
  echo "the bridge directory must contain WineSidecarProbe.exe and the native ELF helper" >&2
  exit 2
fi
for variable in PROTON_STABLE PROTON_EXPERIMENTAL PROTON_GE; do
  if [[ -z ${!variable:-} || ! -f ${!variable} ]]; then
    echo "$variable does not name a Proton launcher" >&2
    exit 2
  fi
done

steam_root="${STEAM_CLIENT_ROOT:-$HOME/.steam/root}"
if [[ ! -d "$steam_root" ]]; then
  echo "STEAM_CLIENT_ROOT is not a Steam installation: $steam_root" >&2
  exit 2
fi
test_root="$(mktemp -d -p "${TMPDIR:-/tmp}" srs-proton-matrix.XXXXXX)"
if [[ "$test_root" != /tmp/srs-proton-matrix.* &&
      "$test_root" != "${TMPDIR:-/tmp}"/srs-proton-matrix.* ]]; then
  echo "refusing unexpected test root: $test_root" >&2
  exit 2
fi
cleanup() {
  rm -rf -- "$test_root"
}
trap cleanup EXIT

for variable in PROTON_STABLE PROTON_EXPERIMENTAL PROTON_GE; do
  launcher="${!variable}"
  slug="${variable,,}"
  prefix="$test_root/$slug-prefix"
  game="$test_root/$slug-game"
  runtime="$test_root/$slug-runtime"
  mkdir -p "$prefix" "$game" "$runtime"
  cp "$probe" "$runtime/WineSidecarProbe.exe"
  cp "$sidecar" "$runtime/SkyrimRuntimeSwapper.Native"
  chmod 0644 "$runtime/SkyrimRuntimeSwapper.Native"
  windows_game="Z:${game//\//\\}"
  echo "Testing $variable"
  STEAM_COMPAT_APP_ID=489830 \
  STEAM_COMPAT_DATA_PATH="$prefix" \
  STEAM_COMPAT_CLIENT_INSTALL_PATH="$steam_root" \
  WINEDEBUG=-all \
    "$launcher" run "$runtime/WineSidecarProbe.exe" "$windows_game"
done

echo "Proton Stable, Experimental, and GE sidecar compatibility passed"
