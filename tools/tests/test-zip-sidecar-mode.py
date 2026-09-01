#!/usr/bin/env python3

import subprocess
import sys
from pathlib import Path
import stat
import tempfile
import zipfile


SCRIPT = Path(__file__).resolve().parents[1] / "set-zip-sidecar-mode.py"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="srs-zip-mode-") as directory:
        archive = Path(directory) / "package.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as writer:
            sidecar = zipfile.ZipInfo("SkyrimRuntimeSwapper.Native")
            sidecar.create_system = 3
            sidecar.external_attr = (stat.S_IFREG | 0o644) << 16
            writer.writestr(sidecar, b"ELF fixture")
            writer.writestr("README.md", b"unchanged")

        subprocess.run([sys.executable, str(SCRIPT), str(archive)], check=True)
        subprocess.run(
            [sys.executable, str(SCRIPT), "--check", str(archive)], check=True)

        with zipfile.ZipFile(archive, "r") as reader:
            sidecar = reader.getinfo("SkyrimRuntimeSwapper.Native")
            mode = (sidecar.external_attr >> 16) & 0xFFFF
            if sidecar.create_system != 3 or mode != stat.S_IFREG | 0o700:
                return 1
            if reader.read("SkyrimRuntimeSwapper.Native") != b"ELF fixture":
                return 2
            if reader.read("README.md") != b"unchanged":
                return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
