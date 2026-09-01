#!/usr/bin/env python3
"""Invoke and validate one native sidecar file-transport operation."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import struct
import subprocess
import tempfile


MAGIC = 0x50535253
VERSION = 6
MAXIMUM_PAYLOAD = 1024 * 1024
HEADER = struct.Struct("<IHHI32s")
OPERATIONS = {
    "probe": 1,
    "recover": 2,
    "activate_session": 3,
    "activate_persistent": 4,
    "restore_persistent": 5,
}
MODES = {
    "automatic": 0,
    "persistent_only": 1,
    "persistent_with_warning": 2,
    "hard_blocked": 3,
}


def field(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack("<I", len(encoded)) + encoded


def take_integer(payload: memoryview, offset: int) -> tuple[int, int]:
    if offset + 4 > len(payload):
        raise AssertionError("the sidecar response ended inside an integer")
    return struct.unpack_from("<I", payload, offset)[0], offset + 4


def take_signed_integer(payload: memoryview, offset: int) -> tuple[int, int]:
    if offset + 4 > len(payload):
        raise AssertionError("the sidecar response ended inside an integer")
    return struct.unpack_from("<i", payload, offset)[0], offset + 4


def take_string(payload: memoryview, offset: int) -> tuple[str, int]:
    size, offset = take_integer(payload, offset)
    if size > MAXIMUM_PAYLOAD or offset + size > len(payload):
        raise AssertionError("the sidecar response contains an invalid string")
    value = bytes(payload[offset:offset + size]).decode("utf-8")
    if "\0" in value:
        raise AssertionError("the sidecar response contains an embedded NUL")
    return value, offset + size


def parse_response(response: bytes, operation: int, nonce: bytes) -> dict[str, object]:
    if len(response) < HEADER.size:
        raise AssertionError("the sidecar response is truncated")
    magic, version, returned_operation, size, returned_nonce = HEADER.unpack_from(response)
    payload = memoryview(response)[HEADER.size:]
    if (
        magic != MAGIC
        or version != VERSION
        or returned_operation != operation
        or returned_nonce != nonce
        or size != len(payload)
        or size > MAXIMUM_PAYLOAD
    ):
        raise AssertionError("the sidecar response is not nonce-bound or well framed")

    offset = 0
    code, offset = take_signed_integer(payload, offset)
    mode, offset = take_integer(payload, offset)
    flags, offset = take_integer(payload, offset)
    allowed_operations, offset = take_integer(payload, offset)
    lifecycle_state, offset = take_integer(payload, offset)
    lifecycle_phase, offset = take_integer(payload, offset)
    installation, offset = take_string(payload, offset)
    path_syntax, offset = take_integer(payload, offset)
    vault, offset = take_string(payload, offset)
    target_cache, offset = take_string(payload, offset)
    coordination_lock, offset = take_string(payload, offset)
    transaction_work, offset = take_string(payload, offset)
    target_id, offset = take_string(payload, offset)
    target_filesystem, offset = take_string(payload, offset)
    target_medium, offset = take_integer(payload, offset)
    target_flags, offset = take_integer(payload, offset)
    vault_id, offset = take_string(payload, offset)
    vault_filesystem, offset = take_string(payload, offset)
    vault_medium, offset = take_integer(payload, offset)
    vault_flags, offset = take_integer(payload, offset)
    target_description, offset = take_string(payload, offset)
    vault_description, offset = take_string(payload, offset)
    backend_description, offset = take_string(payload, offset)
    technical_reason, offset = take_string(payload, offset)
    technical_detail, offset = take_string(payload, offset)
    message, offset = take_string(payload, offset)
    if offset != len(payload):
        raise AssertionError("the sidecar response has trailing data")
    return {
        "code": code,
        "mode": mode,
        "flags": flags,
        "allowed_operations": allowed_operations,
        "lifecycle_state": lifecycle_state,
        "lifecycle_phase": lifecycle_phase,
        "installation": installation,
        "path_syntax": path_syntax,
        "vault": vault,
        "target_cache": target_cache,
        "coordination_lock": coordination_lock,
        "transaction_work": transaction_work,
        "target_id": target_id,
        "target_filesystem": target_filesystem,
        "target_medium": target_medium,
        "target_flags": target_flags,
        "vault_id": vault_id,
        "vault_filesystem": vault_filesystem,
        "vault_medium": vault_medium,
        "vault_flags": vault_flags,
        "target_description": target_description,
        "vault_description": vault_description,
        "backend_description": backend_description,
        "technical_reason": technical_reason,
        "technical_detail": technical_detail,
        "message": message,
        "persistent": bool(flags & 2),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("sidecar", type=pathlib.Path)
    parser.add_argument("operation", choices=OPERATIONS)
    parser.add_argument("game_root", type=pathlib.Path)
    parser.add_argument("catalog", type=pathlib.Path)
    parser.add_argument("--risk-accepted", action="store_true")
    parser.add_argument("--allow-persistent-fallback", action="store_true")
    parser.add_argument("--expect-mode", choices=MODES)
    persistent = parser.add_mutually_exclusive_group()
    persistent.add_argument("--expect-persistent", action="store_true")
    persistent.add_argument("--expect-not-persistent", action="store_true")
    args = parser.parse_args()

    sidecar = args.sidecar.resolve(strict=True)
    game_root = args.game_root.resolve(strict=True)
    catalog = args.catalog.absolute()
    operation = OPERATIONS[args.operation]
    nonce = os.urandom(32)
    payload = (
        field(str(game_root))
        + field(str(catalog))
        + bytes([1 if args.risk_accepted else 0])
        + bytes([1 if args.allow_persistent_fallback else 0])
    )
    request = HEADER.pack(MAGIC, VERSION, operation, len(payload), nonce) + payload

    test_root = pathlib.Path(os.environ.get("SRS_TEST_ROOT", sidecar.parent))
    with tempfile.TemporaryDirectory(prefix="srs-sidecar-invoke-", dir=test_root) as root:
        ipc = pathlib.Path(root)
        ipc.chmod(0o700)
        request_path = ipc / "request.bin"
        response_path = ipc / "response.bin"
        request_path.write_bytes(request)
        request_path.chmod(0o600)
        completed = subprocess.run(
            [sidecar, request_path, response_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=900,
        )
        if completed.returncode != 0:
            raise AssertionError(
                f"sidecar transport failed with {completed.returncode}: "
                f"{completed.stderr.decode(errors='replace')}"
            )
        result = parse_response(response_path.read_bytes(), operation, nonce)

    if result["code"] != 0:
        raise AssertionError(
            f"sidecar operation failed with {result['code']}: "
            f"{result['technical_reason']} {result['message']} "
            f"(target filesystem={result['target_filesystem']!r}, "
            f"target id={result['target_id']!r}, "
            f"target flags={result['target_flags']}, "
            f"vault filesystem={result['vault_filesystem']!r})"
        )
    if args.expect_mode is not None and result["mode"] != MODES[args.expect_mode]:
        raise AssertionError(
            f"sidecar mode was {result['mode']} instead of {MODES[args.expect_mode]}"
        )
    if args.expect_persistent and not result["persistent"]:
        raise AssertionError("sidecar operation did not retain persistent state")
    if args.expect_not_persistent and result["persistent"]:
        raise AssertionError("sidecar operation retained unexpected persistent state")
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
