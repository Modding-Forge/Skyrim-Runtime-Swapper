# Third-party patching boundary

Skyrim Runtime Swapper uses the HDiffPatch 5.1.3 source tree from
<https://github.com/sisong/HDiffPatch5>. The outer checkout is pinned to
`f33c2e3918cc1f5b915f9ac5dd636f8d41dfc72d`; its HDiffPatch engine is pinned to
`3b9dca715ca492873bf2c49e22e5d5b7d2a78620`. All nested compression and checksum
repositories are pinned by commit and checked by
`tools/tests/verify-third-party-pins.py` in CI.

Only the HDIFFW26, Zstandard and xxHash128 paths required by the embedded,
hash-verified runtime manifests are compiled. The SRS adapter supplies
already-opened source, patch and
output streams, applies bounds checks at the adapter boundary, and verifies the
manifest SHA-256 values outside HDiffPatch. Upstream code is built with ASan and
UBSan in the Linux security job and exercised with a generated regression
corpus plus a time-bounded libFuzzer target.

The bundled license and copyright notices remain in:

- `third_party/HDiffPatch5/HDiffPatch/LICENSE` for HDiffPatch (MIT)
- `third_party/HDiffPatch5/zstd/LICENSE` for Zstandard (BSD 3-Clause)
- `third_party/HDiffPatch5/xxHash/LICENSE` for xxHash (BSD 2-Clause)

The release archives include this notice and complete copies of all three
licenses under `THIRD_PARTY-LICENSES/`.
