from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path


SOURCE_VERSION = "1.7.104"
TARGET_VERSION = "1.6.640"
SOURCE_MANIFESTS = {
    "489831": "4940892828028256588",
    "489832": "5728778377666085157",
    "489833": "4886117324142477814",
}
TARGET_MANIFESTS = {
    "489831": "3660787314279169352",
    "489832": "2756691988703496654",
    "489833": "5291801952219815735",
}
FILES = (
    ("SkyrimSE.exe", "exe"),
    ("SkyrimSELauncher.exe", "core"),
    ("Data/Skyrim - Shaders.bsa", "core"),
    ("Data/Skyrim - Interface.bsa", "core"),
    ("Data/Skyrim.esm", "core"),
    ("Data/Update.esm", "core"),
    ("Data/Dawnguard.esm", "core"),
    ("Data/HearthFires.esm", "core"),
    ("Data/Dragonborn.esm", "core"),
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


def patch_entry(
    index: int,
    relative: str,
    source: Path,
    target: Path,
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

    source_hash = sha256(source)
    target_hash = sha256(target)
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
        if sha256(reconstructed_target) != target_hash:
            raise RuntimeError(f"Forward reconstruction mismatch: {relative}")
        if sha256(reconstructed_source) != source_hash:
            raise RuntimeError(f"Reverse reconstruction mismatch: {relative}")

    return {
        "path": relative,
        "sourcePresent": True,
        "targetPresent": True,
        "sourceSha256": source_hash,
        "targetSha256": target_hash,
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
        description="Create the verified Skyrim 1.6.640 hybrid runtime asset catalog"
    )
    parser.add_argument(
        "--source-game-root",
        type=Path,
        required=True,
        help="Verified Skyrim 1.7.104 installation used as the source",
    )
    parser.add_argument(
        "--depot-root",
        type=Path,
        default=workspace / "Skyrim-Runtime-Patch-Creator" / "depots",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=repository / "assets" / "runtime" / "1.7.104-to-1.6.640",
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

    target_exe = args.depot_root / "skyrimse-1.6.640-exe"
    target_core = args.depot_root / "skyrimse-1.6.640-core"
    inputs: list[tuple[str, Path, Path]] = []
    for relative, depot in FILES:
        source = args.source_game_root / Path(relative)
        target_root = target_exe if depot == "exe" else target_core
        target = target_root / Path(relative)
        if not source.is_file() or not target.is_file():
            raise FileNotFoundError(f"Missing verified input: {source} or {target}")
        inputs.append((relative, source, target))

    if args.output.exists():
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True)
    entries = [
        patch_entry(index, relative, source, target, args.output, hdiffz, hpatchz)
        for index, (relative, source, target) in enumerate(inputs, 1)
    ]
    manifest = {
        "format": 3,
        "algorithm": "hdiffpatch-hdiffw26-zstd",
        "hdiffPatchVersion": "5.1.3",
        "variant": "boaw",
        "gameId": "skyrimse",
        "appId": "489830",
        "sourceVersion": SOURCE_VERSION,
        "targetVersion": TARGET_VERSION,
        "sourceManifests": SOURCE_MANIFESTS,
        "targetManifests": TARGET_MANIFESTS,
        "files": entries,
    }
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(args.output / "manifest.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
