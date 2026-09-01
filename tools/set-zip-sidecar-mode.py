#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import stat
import tempfile
import zipfile


SIDECAR_ENTRY = "SkyrimRuntimeSwapper.Native"
SIDECAR_MODE = stat.S_IFREG | 0o700


def sidecar_mode(archive: Path) -> int:
    with zipfile.ZipFile(archive, "r") as reader:
        entries = [entry for entry in reader.infolist()
                   if entry.filename == SIDECAR_ENTRY]
        if len(entries) != 1:
            raise ValueError(f"expected one {SIDECAR_ENTRY} entry")
        entry = entries[0]
        if entry.create_system != 3:
            return 0
        return (entry.external_attr >> 16) & 0xFFFF


def set_sidecar_mode(archive: Path) -> None:
    if not archive.is_file():
        raise ValueError(f"archive does not exist: {archive}")

    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
                prefix=archive.name + ".", suffix=".tmp",
                dir=archive.parent, delete=False) as temporary:
            temporary_path = Path(temporary.name)

        with zipfile.ZipFile(archive, "r") as reader, \
                zipfile.ZipFile(temporary_path, "w", allowZip64=True) as writer:
            matches = 0
            writer.comment = reader.comment
            for entry in reader.infolist():
                contents = reader.read(entry)
                if entry.filename == SIDECAR_ENTRY:
                    matches += 1
                    entry.create_system = 3
                    entry.external_attr = SIDECAR_MODE << 16
                writer.writestr(entry, contents,
                                compress_type=entry.compress_type)
            if matches != 1:
                raise ValueError(f"expected one {SIDECAR_ENTRY} entry")

        with temporary_path.open("r+b") as temporary:
            os.fsync(temporary.fileno())
        os.replace(temporary_path, archive)
        temporary_path = None
        if sidecar_mode(archive) != SIDECAR_MODE:
            raise ValueError("sidecar executable mode verification failed")
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Set and verify the Linux sidecar mode in an SRS ZIP.")
    parser.add_argument("archive", type=Path)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    try:
        if arguments.check:
            if sidecar_mode(arguments.archive) != SIDECAR_MODE:
                raise ValueError("sidecar is not stored as a 0700 regular file")
        else:
            set_sidecar_mode(arguments.archive)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
