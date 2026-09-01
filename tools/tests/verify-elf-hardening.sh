#!/usr/bin/env bash
set -euo pipefail

binary=${1:?usage: verify-elf-hardening.sh <elf>}
headers=$(readelf -W -h -l "$binary")
dynamic=$(readelf -W -d "$binary")
symbols=$(readelf -W -s "$binary")

grep -Eq 'Type:[[:space:]]+DYN' <<<"$headers"
grep -Eq 'GNU_STACK[[:space:]].*RW[[:space:]]' <<<"$headers"
! grep -Eq 'GNU_STACK[[:space:]].*RWE' <<<"$headers"
grep -q 'GNU_RELRO' <<<"$headers"
grep -q 'BIND_NOW' <<<"$dynamic"
grep -q '__stack_chk_fail' <<<"$symbols"
grep -Eq '__[[:alnum:]_]+_chk@GLIBC' <<<"$symbols"
