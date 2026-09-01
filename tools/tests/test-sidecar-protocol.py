#!/usr/bin/env python3
"""Negative and framing tests for the one-shot native sidecar protocol."""

from __future__ import annotations

import fcntl
import os
import pathlib
import struct
import subprocess
import sys
import tempfile


MAGIC = 0x50535253
VERSION = 5
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


def validate_response(response: bytes, operation: int, nonce: bytes,
                      *, require_failure: bool = True) -> int:
    if len(response) < HEADER.size:
        raise AssertionError("valid frame was not answered")
    magic, version, returned_operation, size, response_nonce = HEADER.unpack_from(response)
    response_payload = response[HEADER.size:]
    if (
        magic != MAGIC
        or version != VERSION
        or returned_operation != operation
        or response_nonce != nonce
        or size != len(response_payload)
        or size > MAXIMUM_PAYLOAD
    ):
        raise AssertionError("response nonce binding or framing mismatch")
    if len(response_payload) < 4:
        raise AssertionError("response does not contain an exit code")
    (exit_code,) = struct.unpack_from("<i", response_payload)
    if require_failure and exit_code == 0:
        raise AssertionError("an unknown operation was accepted")
    return exit_code


def run_file_transport(sidecar: pathlib.Path, directory: pathlib.Path,
                       request: bytes, *, env: dict[str, str] | None = None
                       ) -> subprocess.CompletedProcess[bytes]:
    directory.mkdir(mode=0o700)
    request_path = directory / "request.bin"
    response_path = directory / "response.bin"
    request_path.write_bytes(request)
    request_path.chmod(0o666)
    return subprocess.run(
        [sidecar, request_path, response_path], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False, timeout=15, env=env,
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

    with tempfile.TemporaryDirectory(
        prefix="srs-sidecar-protocol-", dir=sidecar.parent
    ) as root:
        game = pathlib.Path(root, "game").resolve()
        catalog = pathlib.Path(root, "state", "ContentCatalog.txt").resolve()
        nonce = os.urandom(32)
        payload = field(str(game)) + field(str(catalog)) + b"\0\0"
        result = run(sidecar, frame(0xFFFF, payload, nonce))
        if result.returncode != 0 or len(result.stdout) < HEADER.size:
            raise AssertionError(
                f"valid frame was not answered: {result.returncode}, {result.stderr!r}"
            )
        validate_response(result.stdout, 0xFFFF, nonce)

        file_nonce = os.urandom(32)
        file_request = frame(0xFFFF, payload, file_nonce)
        ipc = pathlib.Path(root, "file-ipc")
        result = run_file_transport(sidecar, ipc, file_request)
        response_path = ipc / "response.bin"
        if result.returncode != 0 or result.stdout or not response_path.is_file():
            raise AssertionError(
                f"file transport failed: {result.returncode}, "
                f"stdout={result.stdout!r}, stderr={result.stderr!r}"
            )
        validate_response(response_path.read_bytes(), 0xFFFF, file_nonce)
        if (ipc.stat().st_mode & 0o777) != 0o700:
            raise AssertionError("file transport directory was not restricted")
        if ((ipc / "request.bin").stat().st_mode & 0o777) != 0o600:
            raise AssertionError("file transport request was not restricted")
        if (response_path.stat().st_mode & 0o777) != 0o600:
            raise AssertionError("file transport response was not restricted")

        trailing = pathlib.Path(root, "trailing-request-data")
        result = run_file_transport(sidecar, trailing, file_request + b"junk")
        if result.returncode != 2 or (trailing / "response.bin").exists():
            raise AssertionError("trailing request data was accepted")

        collision = pathlib.Path(root, "response-collision")
        collision.mkdir(mode=0o700)
        (collision / "request.bin").write_bytes(file_request)
        (collision / "response.bin").write_bytes(b"untrusted")
        result = subprocess.run(
            [sidecar, collision / "request.bin", collision / "response.bin"],
            check=False, timeout=15,
        )
        if result.returncode != 12 or (collision / "response.bin").read_bytes() != b"untrusted":
            raise AssertionError("a pre-existing response file was overwritten")

        linked = pathlib.Path(root, "linked-request")
        linked.mkdir(mode=0o700)
        real_request = linked / "real-request.bin"
        real_request.write_bytes(file_request)
        os.link(real_request, linked / "request.bin")
        result = subprocess.run(
            [sidecar, linked / "request.bin", linked / "response.bin"],
            check=False, timeout=15,
        )
        if result.returncode != 11:
            raise AssertionError("a hard-linked request file was accepted")

        symlinked = pathlib.Path(root, "symlinked-request")
        symlinked.mkdir(mode=0o700)
        real_request = symlinked / "real-request.bin"
        real_request.write_bytes(file_request)
        os.symlink(real_request, symlinked / "request.bin")
        result = subprocess.run(
            [sidecar, symlinked / "request.bin", symlinked / "response.bin"],
            check=False, timeout=15,
        )
        if result.returncode != 11:
            raise AssertionError("a symbolic-link request file was accepted")

        real_directory = pathlib.Path(root, "real-ipc-directory")
        real_directory.mkdir(mode=0o700)
        (real_directory / "request.bin").write_bytes(file_request)
        directory_link = pathlib.Path(root, "linked-ipc-directory")
        os.symlink(real_directory, directory_link)
        result = subprocess.run(
            [sidecar, directory_link / "request.bin", directory_link / "response.bin"],
            check=False, timeout=15,
        )
        if result.returncode != 10:
            raise AssertionError("a symbolic-link IPC directory was accepted")

        lock_game = pathlib.Path(root, "lock-game")
        lock_game.mkdir(mode=0o700)
        lock_environment = os.environ.copy()
        lock_environment["XDG_STATE_HOME"] = str(pathlib.Path(root, "state"))
        lock_environment["HOME"] = str(pathlib.Path(root, "home"))
        pathlib.Path(lock_environment["XDG_STATE_HOME"]).mkdir(mode=0o700)
        pathlib.Path(lock_environment["HOME"]).mkdir(mode=0o700)
        lock_catalog = pathlib.Path(root, "catalog", "ContentCatalog.txt")
        lock_payload = (
            field(str(lock_game.resolve()))
            + field(str(lock_catalog.resolve()))
            + b"\0\0"
        )
        prepare_nonce = os.urandom(32)
        prepare_ipc = pathlib.Path(root, "prepare-launch-operation")
        prepare_result = run_file_transport(
            sidecar, prepare_ipc, frame(6, lock_payload, prepare_nonce),
            env=lock_environment,
        )
        if prepare_result.returncode != 0:
            raise AssertionError("prepare_launch did not return a framed result")
        validate_response(
            (prepare_ipc / "response.bin").read_bytes(), 6, prepare_nonce
        )

        seed_nonce = os.urandom(32)
        seed_ipc = pathlib.Path(root, "seed-lock-operation")
        seed_result = run_file_transport(
            sidecar, seed_ipc, frame(2, lock_payload, seed_nonce),
            env=lock_environment,
        )
        if seed_result.returncode != 0:
            raise AssertionError("the external coordination lock was not created")
        lock_files = list(
            pathlib.Path(lock_environment["XDG_STATE_HOME"]).glob(
                "modding-forge/skyrim-runtime-swapper/locks/*.lock"
            )
        )
        if len(lock_files) != 1:
            raise AssertionError("the resolved coordination lock is ambiguous")
        lock_descriptor = os.open(lock_files[0], os.O_RDWR)
        try:
            fcntl.flock(lock_descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            locked_nonce = os.urandom(32)
            locked_ipc = pathlib.Path(root, "locked-operation")
            result = run_file_transport(
                sidecar, locked_ipc, frame(2, lock_payload, locked_nonce),
                env=lock_environment,
            )
            if result.returncode != 0:
                raise AssertionError("the concurrent operation did not answer safely")
            locked_response = (locked_ipc / "response.bin").read_bytes()
            locked_exit_code = validate_response(locked_response, 2, locked_nonce)
            if locked_exit_code != 28:
                raise AssertionError("a concurrent native operation bypassed its lock")
        finally:
            os.close(lock_descriptor)

        stale_nonce = os.urandom(32)
        stale_ipc = pathlib.Path(root, "stale-lock-operation")
        result = run_file_transport(
            sidecar, stale_ipc, frame(2, lock_payload, stale_nonce),
            env=lock_environment,
        )
        if result.returncode != 0:
            raise AssertionError("the stale-lock recovery did not answer safely")
        stale_response = (stale_ipc / "response.bin").read_bytes()
        stale_exit_code = validate_response(
            stale_response, 2, stale_nonce, require_failure=False
        )
        if stale_exit_code == 28:
            raise AssertionError("a stale lock file blocked native recovery")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
