#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


LIMITS = {
    "GLIBC": (2, 35),
    "GLIBCXX": (3, 4, 29),
    "CXXABI": (1, 3, 13),
}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: verify-elf-abi.py <elf>", file=sys.stderr)
        return 2
    binary = pathlib.Path(sys.argv[1])
    output = subprocess.run(
        ["readelf", "--version-info", binary],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout
    for namespace, limit in LIMITS.items():
        versions = {
            tuple(int(part) for part in match.split("."))
            for match in re.findall(rf"\b{namespace}_([0-9]+(?:\.[0-9]+)+)\b", output)
        }
        if versions and max(versions) > limit:
            actual = ".".join(map(str, max(versions)))
            allowed = ".".join(map(str, limit))
            raise SystemExit(
                f"{binary} requires {namespace}_{actual}; maximum is {namespace}_{allowed}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
