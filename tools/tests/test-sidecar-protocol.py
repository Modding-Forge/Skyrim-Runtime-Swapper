#!/usr/bin/env python3
"""Negative and framing tests for the one-shot native sidecar protocol."""

from __future__ import annotations

import os
import pathlib
import struct
import subprocess
import sys
import tempfile


MAGIC = 0x50535253
VERSION = 2
HEADER = struct.Struct("<IHHI32s")
MAXIMUM_PAYLOAD = 1024 * 1024


def run(sidecar: pathlib.Path, request: bytes) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [sidecar], input=request, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False, timeout=15,
    )


def frame(operation: int, payload: bytes, nonce: bytes | None = None,
          *, magic: int = MAGIC, version: int = VERSION,
          declared_size: int | None = None) -> bytes:
    nonce = nonce if nonce is not None else os.urandom(32)
    size = len(payload) if declared_size is None else declared_size
    return HEADER.pack(magic, version, operation, size, nonce) + payload


def field(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack("<I", len(encoded)) + encoded


def expect_rejected(sidecar: pathlib.Path, request: bytes, expected: int) -> None:
    result = run(sidecar, request)
    if result.returncode != expected or result.stdout:
        raise AssertionError(
            f"expected rejection {expected}, got {result.returncode}; "
            f"stdout={result.stdout!r} stderr={result.stderr!r}"
        )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test-sidecar-protocol.py <SkyrimRuntimeSwapper.Native>",
              file=sys.stderr)
        return 2
    sidecar = pathlib.Path(sys.argv[1]).resolve()
    if not sidecar.is_file():
        raise FileNotFoundError(sidecar)

    expect_rejected(sidecar, b"short", 2)
    expect_rejected(sidecar, frame(1, b"", magic=0), 2)
    expect_rejected(sidecar, frame(1, b"", version=VERSION + 1), 2)
    expect_rejected(sidecar, frame(1, b"", nonce=b"\0" * 32), 2)
    expect_rejected(
        sidecar,
        frame(1, b"", declared_size=MAXIMUM_PAYLOAD + 1),
        2,
    )
    expect_rejected(sidecar, frame(1, b""), 4)

    with tempfile.TemporaryDirectory(prefix="srs-sidecar-protocol-") as root:
        game = pathlib.Path(root, "game").resolve()
        catalog = pathlib.Path(root, "state", "ContentCatalog.txt").resolve()
        nonce = os.urandom(32)
        payload = field(str(game)) + field(str(catalog)) + b"\0"
        result = run(sidecar, frame(0xFFFF, payload, nonce))
        if result.returncode != 0 or len(result.stdout) < HEADER.size:
            raise AssertionError(
                f"valid frame was not answered: {result.returncode}, {result.stderr!r}"
            )
        magic, version, operation, size, response_nonce = HEADER.unpack_from(result.stdout)
        response_payload = result.stdout[HEADER.size:]
        if (
            magic != MAGIC
            or version != VERSION
            or operation != 0xFFFF
            or response_nonce != nonce
            or size != len(response_payload)
            or size > MAXIMUM_PAYLOAD
        ):
            raise AssertionError("response authentication or framing mismatch")
        if len(response_payload) < 4:
            raise AssertionError("response does not contain an exit code")
        (exit_code,) = struct.unpack_from("<i", response_payload)
        if exit_code == 0:
            raise AssertionError("an unknown operation was accepted")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
