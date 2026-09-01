#!/usr/bin/env python3
"""Fail closed when the recursively pinned HDiffPatch dependency drifts."""

from __future__ import annotations

import pathlib
import subprocess
import sys


EXPECTED = {
    "third_party/HDiffPatch5": "f33c2e3918cc1f5b915f9ac5dd636f8d41dfc72d",
    "third_party/HDiffPatch5/HDiffPatch": "3b9dca715ca492873bf2c49e22e5d5b7d2a78620",
    "third_party/HDiffPatch5/bzip2": "fbc4b11da543753b3b803e5546f56e26ec90c2a7",
    "third_party/HDiffPatch5/libdeflate": "8f894fab60464e13ddee3846eae722216347fac1",
    "third_party/HDiffPatch5/libmd5": "51edeb63ec3f456f4950922c5011c326a062fbce",
    "third_party/HDiffPatch5/lzma": "44809544b95bbf2dccec75d954657ff76fc349ff",
    "third_party/HDiffPatch5/xxHash": "e626a72bc2321cd320e953a0ccf1584cad60f363",
    "third_party/HDiffPatch5/zlib": "323357a50daba38cedd2b766b3427f4c6d33b10f",
    "third_party/HDiffPatch5/zstd": "68c88c7c7ad22b5e6882a5296ef96d27dc8750c4",
}


def git(root: pathlib.Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return completed.stdout.strip()


def main() -> int:
    repository = pathlib.Path(__file__).resolve().parents[2]
    failures: list[str] = []
    for relative, expected in EXPECTED.items():
        checkout = repository / relative
        if not (checkout / ".git").exists():
            failures.append(f"{relative}: submodule is not initialized")
            continue
        try:
            actual = git(checkout, "rev-parse", "HEAD")
            dirty = git(checkout, "status", "--porcelain", "--untracked-files=no")
        except subprocess.CalledProcessError as error:
            failures.append(f"{relative}: git inspection failed: {error.stderr.strip()}")
            continue
        if actual != expected:
            failures.append(f"{relative}: expected {expected}, found {actual}")
        if dirty:
            failures.append(f"{relative}: tracked files differ from the pinned commit")

    if failures:
        print("Third-party pin verification failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print(f"Verified {len(EXPECTED)} recursively pinned HDiffPatch repositories.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
