#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run-linux-runtime-fault-matrix.sh <debug-runtime-probe> <baseline-game-root> <patch-root>" >&2
  exit 2
fi

probe="$(realpath "$1")"
baseline="$(realpath "$2")"
patches="$(realpath "$3")"
if [[ ! -x "$probe" || ! -d "$baseline" || ! -d "$patches" ]]; then
  echo "the probe, baseline, or patch directory is unavailable" >&2
  exit 2
fi

for command in cmp cp find realpath timeout; do
  command -v "$command" >/dev/null || {
    echo "missing test dependency: $command" >&2
    exit 2
  }
done

test_base="${SRS_TEST_ROOT_BASE:-${XDG_STATE_HOME:-$HOME/.local/state}}"
mkdir -p -m 0700 "$test_base"
test_base="$(realpath "$test_base")"
test_root="$(mktemp -d -p "$test_base" srs-runtime-faults.XXXXXX)"
if [[ "$test_root" != "$test_base"/srs-runtime-faults.* ]]; then
  echo "refusing unexpected test root: $test_root" >&2
  exit 2
fi
cleanup() {
  if [[ ${SRS_KEEP_TEST_FIXTURES:-0} == 1 ]]; then
    echo "Fault matrix fixtures retained: $test_root"
  else
    rm -rf -- "$test_root"
  fi
}
trap cleanup EXIT

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

expect_crash() {
  local game="$1"
  local state="$2"
  local phase="$3"
  set +e
  XDG_STATE_HOME="$state" HOME="$test_root/home" \
    SKYRIM_RUNTIME_SWAPPER_FAULT_AFTER_PHASE="$phase" \
    timeout 300 "$probe" "$game" "$patches" >/dev/null 2>&1
  local result=$?
  set -e
  if [[ $result -ne 201 ]]; then
    echo "journal phase $phase did not terminate the Debug probe (exit $result)" >&2
    return 1
  fi
}

for phase in {1..10}; do
  game="$test_root/phase-$phase/game"
  state="$test_root/phase-$phase/state"
  mkdir -p "$game"
  mkdir -m 0700 "$state"
  cp -a -- "$baseline"/. "$game"/

  # Later phases require an existing pending transaction so recovery paths are
  # exercised before the next injected interruption.
  if [[ $phase -ge 8 ]]; then
    expect_crash "$game" "$state" 3
  fi
  expect_crash "$game" "$state" "$phase"

  XDG_STATE_HOME="$state" HOME="$test_root/home" \
    timeout 300 "$probe" "$game" "$patches" >/dev/null
  verify_baseline "$game"
  XDG_STATE_HOME="$state" HOME="$test_root/home" \
    timeout 300 "$probe" "$game" "$patches" >/dev/null
  verify_baseline "$game"
  echo "journal phase $phase recovered idempotently"
done

echo "Linux runtime fault and recovery matrix passed"
