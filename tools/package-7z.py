"""Build and round-trip verify deterministic 7z packages with POSIX modes.

Windows delegates to WSL because Windows archive writers cannot reliably
capture the native sidecar's Unix execute bits. No installed Python packages
are needed. The encoder is built from the repository's pinned LZMA SDK.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[1]
NATIVE = "SkyrimRuntimeSwapper.Native"
OPTIONS = ["-t7z", "-m0=LZMA2", "-mx=9", "-md=64m", "-mmt=1",
           "-ms=on", "-mtc=off", "-mta=off", "-mtm=off", "-bd", "-bb0"]


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def encoder() -> Path:
    binary = REPO / "build/tools/7zr"
    source = REPO / "third_party/HDiffPatch5/lzma"
    # Rebuild if any pinned SDK input has changed since the cached tool.
    inputs = [p for root in (source / "C", source / "CPP")
              for p in root.rglob("*") if p.is_file()]
    if not binary.is_file() or any(p.stat().st_mtime > binary.stat().st_mtime for p in inputs):
        with tempfile.TemporaryDirectory(prefix="srs-7zr-build-") as temp:
            subprocess.run(["make", "-f", "makefile.gcc", "-j8", f"O={temp}"],
                           cwd=source / "CPP/7zip/Bundles/Alone7z", check=True)
            binary.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(Path(temp) / "7zr", binary)
            binary.chmod(0o700)
    return binary


def payload(root: Path) -> dict[str, str]:
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError(f"Unsupported package entry: {path}")
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            if "\n" in relative or "\r" in relative:
                raise ValueError("Archive filenames must not contain newlines")
            result[relative] = digest(path)
    return result


def package(source: Path, output: Path, tool: Path) -> None:
    if not source.is_dir() or source.is_symlink():
        raise ValueError("Source must be a plain package directory")
    if output.suffix != ".7z" or output.exists():
        raise ValueError("Output must be a new .7z file; existing archives are never overwritten")
    if source == output.parent or source in output.parents:
        raise ValueError("Archive must be outside the package input directory")
    expected = payload(source)
    if not expected:
        raise ValueError("Cannot package an empty directory")
    with tempfile.TemporaryDirectory(prefix="srs-7z-package-") as temp:
        work = Path(temp)
        staged = work / "payload"
        staged.mkdir()
        for name in expected:
            target = staged / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / name, target)
            target.chmod(0o700 if name == NATIVE else 0o644)
            os.utime(target, (0, 0))
        if payload(staged) != expected:
            raise ValueError("Source changed while preparing the package")
        file_list = work / "files.txt"
        file_list.write_text("\n".join(expected) + "\n", encoding="utf-8")
        archive = work / "package.7z"
        subprocess.run([str(tool), "a", str(archive), *OPTIONS, f"@{file_list}"],
                       cwd=staged, check=True)
        extracted = work / "extracted"
        subprocess.run([str(tool), "x", str(archive), f"-o{extracted}", "-y", "-bd", "-bb0"],
                       check=True)
        if payload(extracted) != expected:
            raise ValueError("Extracted package contents do not match the source hashes")
        if NATIVE in expected and stat.S_IMODE((extracted / NATIVE).stat().st_mode) != 0o700:
            raise ValueError("The 7z archive did not preserve native sidecar mode 0700")
        if payload(source) != expected:
            raise ValueError("Source changed during archive creation")
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("xb") as destination, archive.open("rb") as stream:
            shutil.copyfileobj(stream, destination)
        receipt = {"format": "7z", "encoderSha256": digest(tool), "options": OPTIONS,
                   "archiveSha256": digest(output), "files": expected,
                   "nativeMode": "0700" if NATIVE in expected else None,
                   "extractedHashesVerified": True}
        output.with_suffix(".7z.build.json").write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def self_test(tool: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="srs-7z-check-") as temp:
        root = Path(temp)
        source = root / "input"
        (source / "RuntimeSwap").mkdir(parents=True)
        (source / NATIVE).write_bytes(b"native fixture\n")
        (source / "version.dll").write_bytes(b"dll fixture\n")
        (source / "RuntimeSwap/manifest.json").write_text("{}\n", encoding="utf-8")
        first, second = root / "first.7z", root / "second.7z"
        package(source, first, tool)
        os.utime(source / NATIVE, (1700000000, 1700000000))
        package(source, second, tool)
        if digest(first) != digest(second):
            raise ValueError("Identical payloads produced different archives")
        try:
            package(source, first, tool)
        except ValueError:
            pass
        else:
            raise ValueError("Existing archive overwrite was accepted")
    print("7z round-trip, native mode, reproducibility and overwrite checks passed.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--wsl-distribution", default="")
    args = parser.parse_args()
    if not args.self_test and (args.source is None or args.output is None):
        parser.error("--source and --output are required")
    if os.name == "nt":
        wsl = ["wsl.exe"]
        if args.wsl_distribution:
            wsl += ["--distribution", args.wsl_distribution]
        def linux_path(path: Path) -> str:
            return subprocess.check_output(
                [*wsl, "--exec", "wslpath", "-a", str(path.resolve())], text=True).strip()
        command = [*wsl, "--exec", "python3", linux_path(Path(__file__))]
        if args.self_test:
            command += ["--self-test"]
        else:
            command += ["--source", linux_path(args.source), "--output", linux_path(args.output)]
        subprocess.run(command, check=True)
        return
    tool = encoder()
    if args.self_test:
        self_test(tool)
    else:
        package(args.source.resolve(), args.output.resolve(), tool)


if __name__ == "__main__":
    main()
