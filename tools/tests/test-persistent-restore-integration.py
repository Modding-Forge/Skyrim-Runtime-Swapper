#!/usr/bin/env python3
"""Real-file restore/restart regression; requires local, legally obtained fixtures."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
import pathlib
import shutil
import subprocess
import tempfile


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("sidecar", type=pathlib.Path)
    parser.add_argument("baseline", type=pathlib.Path)
    parser.add_argument("patches", type=pathlib.Path)
    parser.add_argument("--old-sidecar", type=pathlib.Path)
    parser.add_argument("--test-base", type=pathlib.Path, required=True)
    parser.add_argument("--cleanup-only", action="store_true")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location(
        "sidecar_protocol", pathlib.Path(__file__).with_name("invoke-sidecar-operation.py"))
    protocol = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(protocol)
    baseline = args.baseline.resolve(strict=True)
    hashes = {p.relative_to(baseline): sha256(p)
              for p in baseline.rglob("*") if p.is_file()}
    with tempfile.TemporaryDirectory(prefix="srs-restore-regression-", dir=args.test_base) as temporary:
        root = pathlib.Path(temporary)
        game = root / "SteamLibrary/steamapps/common/Skyrim Special Edition"
        shutil.copytree(baseline, game)
        shutil.copytree(args.patches, game / "RuntimeSwap/patches")
        catalog = root / "catalog/ContentCatalog.txt"
        catalog.parent.mkdir()
        catalog.write_text("verified test catalog\n", encoding="utf-8")
        state = root / "state"
        state.mkdir(mode=0o700)
        env = os.environ | {"XDG_STATE_HOME": str(state)}

        def invoke(operation: str, *, old: bool = False, expected: int = 0) -> dict:
            native = args.old_sidecar if old else args.sidecar
            nonce = os.urandom(32)
            op = protocol.OPERATIONS[operation]
            payload = protocol.field(str(game)) + protocol.field(str(catalog)) + b"\0\0"
            request = protocol.HEADER.pack(protocol.MAGIC, protocol.VERSION, op, len(payload), nonce) + payload
            completed = subprocess.run([native], input=request, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, env=env, timeout=180, check=False)
            assert completed.returncode == 0, completed.stderr.decode(errors="replace")
            result = protocol.parse_response(completed.stdout, op, nonce)
            assert result["code"] == expected, result
            print(f"{('old' if old else 'new')} {operation}: code={result['code']}, flags={result['flags']}", flush=True)
            return result

        def source_verified() -> None:
            for relative, digest in hashes.items():
                assert sha256(game / relative) == digest, relative
            assert not (game / ".skyrim-runtime-swapper").exists()
            storage = root / "SteamLibrary/.runtime-swapper"
            assert not (storage / "recovery").exists()
            assert not list((storage / "work").glob("*/vault.locator"))

        def resume_cleanup() -> None:
            for phase in ("source_verified", "cleanup_pending"):
                probe = invoke("probe")
                vault = pathlib.Path(probe["vault"])
                assert vault.is_relative_to(root), "fixture vault escaped the temporary root"
                current = root
                for part in (vault / "attachments").relative_to(root).parts:
                    current /= part
                    current.mkdir(mode=0o700, exist_ok=True)
                lifecycle = current / "lifecycle"
                lifecycle.write_text(f"SRS-RECOVERY-LIFECYCLE-1\nstate={phase}\n", encoding="utf-8")
                lifecycle.chmod(0o600)
                invoke("recover")
                source_verified()
                print(f"Interrupted cleanup resumed from {phase}.", flush=True)

        if args.cleanup_only:
            resume_cleanup()
            return

        if args.old_sidecar:
            invoke("activate_persistent", old=True)
            invoke("restore_persistent", old=True)
            failed = invoke("restore_persistent", old=True, expected=27)
            intent = pathlib.Path(failed["vault"]) / "attachments/persistent-restore"
            assert intent.read_text() == "SRS-PERSISTENT-RESTORE-1\n"
            invoke("recover")
            source_verified()

        invoke("activate_persistent")
        invoke("restore_persistent")
        second = invoke("restore_persistent")
        assert not (second["flags"] & 1), "second restore changed managed content"
        source_verified()
        resume_cleanup()
        invoke("prepare_launch")
        invoke("recover")
        source_verified()
        print("Real-file persistent restore, old residue recovery and subsequent launch passed.", flush=True)


if __name__ == "__main__":
    main()
