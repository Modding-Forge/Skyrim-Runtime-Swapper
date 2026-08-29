from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path


SOURCE_VERSION = "1.7.104"
TARGET_VERSION = "1.5.97"
DATA_BASELINE_VERSION = "1.6.1170"
DATA_FILES = (
    "Data/Skyrim - Shaders.bsa",
    "Data/Skyrim - Interface.bsa",
    "Data/Skyrim.esm",
    "Data/Update.esm",
    "Data/Dawnguard.esm",
    "Data/HearthFires.esm",
    "Data/Dragonborn.esm",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(command: list[str], operation: str) -> None:
    result = subprocess.run(command, capture_output=True, text=True, errors="replace")
    if result.returncode:
        details = (result.stdout + result.stderr).strip()
        raise RuntimeError(f"{operation} failed ({result.returncode}): {details}")


def create_patch_entry(
    index: int,
    relative: str,
    source: Path,
    target: Path,
    source_present: bool,
    output: Path,
    hdiffz: Path,
    hpatchz: Path,
) -> dict[str, object]:
    stem = relative.replace("/", "_").replace(" ", "_")
    forward_name = f"{index:04d}_{stem}_forward.hdiff"
    reverse_name = f"{index:04d}_{stem}_reverse.hdiff"
    forward = output / "forward" / forward_name
    reverse = output / "reverse" / reverse_name
    forward.parent.mkdir(parents=True, exist_ok=True)
    reverse.parent.mkdir(parents=True, exist_ok=True)
    common = ["-f", "-WD-256k", "-s-64", "-c-zstd-3", "-C-xxh128", "-p-5"]
    run([str(hdiffz), *common, str(source), str(target), str(forward)], relative)
    run([str(hdiffz), *common, str(target), str(source), str(reverse)], relative)

    with tempfile.TemporaryDirectory(dir=output) as temporary:
        verify = Path(temporary)
        reconstructed_target = verify / "target"
        reconstructed_source = verify / "source"
        run(
            [str(hpatchz), "-f", "-p-5", str(source), str(forward), str(reconstructed_target)],
            f"forward verification for {relative}",
        )
        run(
            [str(hpatchz), "-f", "-p-5", str(target), str(reverse), str(reconstructed_source)],
            f"reverse verification for {relative}",
        )
        if sha256(reconstructed_target) != sha256(target):
            raise RuntimeError(f"Forward reconstruction mismatch: {relative}")
        if sha256(reconstructed_source) != sha256(source):
            raise RuntimeError(f"Reverse reconstruction mismatch: {relative}")

    return {
        "path": relative,
        "sourcePresent": source_present,
        "targetPresent": True,
        "sourceSha256": sha256(source),
        "targetSha256": sha256(target),
        "sourceSize": source.stat().st_size,
        "targetSize": target.stat().st_size,
        "forwardPatch": f"forward/{forward_name}",
        "forwardPatchSha256": sha256(forward),
        "forwardPatchSize": forward.stat().st_size,
        "reversePatch": f"reverse/{reverse_name}",
        "reversePatchSha256": sha256(reverse),
        "reversePatchSize": reverse.stat().st_size,
    }


def main() -> int:
    repository = Path(__file__).resolve().parents[1]
    workspace = repository.parent
    parser = argparse.ArgumentParser(
        description="Create the verified Skyrim 1.5.97 hybrid runtime asset catalog"
    )
    parser.add_argument(
        "--depot-root",
        type=Path,
        default=workspace / "steamcmd-bethesda" / "depots",
    )
    parser.add_argument(
        "--base-manifest",
        type=Path,
        default=repository / "assets" / "runtime" / "1.7.104-to-1.6.1170" / "manifest.json",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=repository / "assets" / "runtime" / "1.7.104-to-1.5.97",
    )
    parser.add_argument(
        "--tool-root",
        type=Path,
        default=workspace / "Skyrim-Runtime-Patch-Creator" / "tools" / "bin",
    )
    args = parser.parse_args()

    hdiffz = args.tool_root / "hdiffz.exe"
    hpatchz = args.tool_root / "hpatchz.exe"
    for tool in (hdiffz, hpatchz):
        if not tool.is_file():
            raise FileNotFoundError(tool)

    source_exe = args.depot_root / "skyrimse-1.7.104-exe"
    source_core = args.depot_root / "skyrimse-1.7.104-core"
    target_exe = args.depot_root / "skyrimse-1.5.97-exe"
    target_core = args.depot_root / "skyrimse-1.5.97-core"
    runtime_files = (
        ("SkyrimSE.exe", source_exe / "SkyrimSE.exe", target_exe / "SkyrimSE.exe", True),
        (
            "SkyrimSELauncher.exe",
            source_exe / "SkyrimSELauncher.exe",
            target_core / "SkyrimSELauncher.exe",
            True,
        ),
        ("steam_api64.dll", source_core / "steam_api64.dll", target_core / "steam_api64.dll", True),
    )
    for _, source, target, _ in runtime_files:
        if not source.is_file() or not target.is_file():
            raise FileNotFoundError(f"Missing verified depot input: {source} or {target}")

    base_manifest = json.loads(args.base_manifest.read_text(encoding="utf-8"))
    base_entries = {entry["path"]: entry for entry in base_manifest["files"]}
    for relative in DATA_FILES:
        if relative not in base_entries:
            raise KeyError(f"The 1.6.1170 data baseline is missing {relative}")

    if args.output.exists():
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True)
    with tempfile.TemporaryDirectory(dir=args.output) as temporary:
        empty = Path(temporary) / "empty"
        empty.write_bytes(b"")
        entries = [
            create_patch_entry(
                index, relative, source, target, source_present, args.output, hdiffz, hpatchz
            )
            for index, (relative, source, target, source_present) in enumerate(runtime_files, 1)
        ]
        entries.append(
            create_patch_entry(
                4,
                "binkw64.dll",
                empty,
                target_core / "binkw64.dll",
                False,
                args.output,
                hdiffz,
                hpatchz,
            )
        )

    for relative in DATA_FILES:
        entry = base_entries[relative]
        entries.append(entry)
        for direction in ("forward", "reverse"):
            patch_name = entry[f"{direction}Patch"]
            source = args.base_manifest.parent / patch_name
            destination = args.output / patch_name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            if sha256(destination) != entry[f"{direction}PatchSha256"]:
                raise RuntimeError(f"Copied patch hash mismatch: {relative} {direction}")

    manifest = {
        "format": 3,
        "algorithm": "hdiffpatch-hdiffw26-zstd",
        "hdiffPatchVersion": "5.1.3",
        "variant": "boaw",
        "gameId": "skyrimse",
        "appId": "489830",
        "sourceVersion": SOURCE_VERSION,
        "targetVersion": TARGET_VERSION,
        "dataBaselineVersion": DATA_BASELINE_VERSION,
        "dataBaselineManifests": base_manifest["targetManifests"],
        "sourceManifests": base_manifest["sourceManifests"],
        "targetManifests": {
            "489831": "7848722008564294070",
            "489832": "8702665189575304780",
            "489833": "2289561010626853674",
        },
        "files": entries,
    }
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(args.output / "manifest.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
