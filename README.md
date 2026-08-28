# Skyrim Runtime Swapper

Skyrim Runtime Swapper lets a Skyrim mod collection built for an older runtime start through the unmodified `skse64_loader.exe`. The original Steam runtime is restored automatically after every game session.

Current release: `1.0.0`.

## Supported runtimes

Every release archive is target-specific and accepts the exact Steam source files for Skyrim `1.7.104`.

| Source | Target |
| --- | --- |
| 1.7.104 | 1.6.1170 |
| 1.7.104 | 1.6.640 |
| 1.7.104 | 1.5.97 |

Other source files, stores, and runtimes are rejected without modification.

## Installation

1. Choose the archive matching the runtime required by the collection.
2. Install it as a normal root mod through the mod manager and deploy it.
3. Verify that `version.dll`, `SkyrimRuntimeSwapper.exe`, and `RuntimeSwap\patches` exist in the Skyrim game directory.
4. Launch the unmodified `skse64_loader.exe` selected by the collection.

Do not combine multiple target archives in one installation.

## How it works

The native `version.dll` proxy is loaded by `skse64_loader.exe` before SKSE checks `SkyrimSE.exe`. It forwards all 17 exports provided by the Windows system DLL and starts `SkyrimRuntimeSwapper.exe` only for the Skyrim runtime check.

The helper validates the runtime and SHA-256 hashes, applies the bundled BSDIFF40 patches to staged copies, validates every result, creates immutable backups, and commits the swap only after all checks pass. Unknown, modified, or incomplete files fail closed.

Every SKSE launch checks the current user's `ContentCatalog.txt`, including launches that already use the target runtime or activate it from the cache. An incompatible catalog is backed up, removed for the game session, and restored afterward.

Visible Runtime Swapper errors include a button that copies the newest `skse64.log` from the current user's Skyrim documents directory to the clipboard.

A detached watcher locates the exact `SkyrimSE.exe` process and waits on its process handle without polling during gameplay. When the process exits, it restores only the files changed during that session. If the target runtime was already installed, game binaries remain untouched and only a temporarily removed `ContentCatalog.txt` is restored. Verified swapped target files remain in a local cache, so later launches use same-volume file moves instead of patching again.

A session barrier remains active until restoration is complete. If SKSE is launched again immediately after Skyrim exits, the new launch waits for the previous watcher before it inspects or swaps any runtime files.

Runtime state is stored in `.skyrim-runtime-swapper` inside the game directory. Stale staging directories are removed automatically.

The bootstrap does not use CommonLibSSE, Address Library, or the SKSE plugin API. It runs before regular SKSE plugins and before the Engine Fixes preloader.

## Building

Requirements:

- Windows x64
- Visual Studio with the Desktop development with C++ workload
- CMake 3.25 or newer
- vcpkg

Run `build.bat` from the repository root. It discovers every manifest below `patches`, creates one target-specific build, runs the native tests, validates binary metadata and patch hashes, checks the packaged runtime path, and writes the archives to `dist/builds/<version>/<build-id>/`.

The bundled patch sets are created with the [SteamCMD Bethesda Patch Builder](https://github.com/Modding-Forge/steamcmd-bethesda). It downloads the configured Steam depots, creates BSDIFF40 patches, reconstructs every target file for verification, and writes the SHA-256 manifests consumed by this repository.

The source and target game files are not part of the repository. Only verified BSDIFF40 patch data and cryptographic hashes are included.

## License

Copyright (c) 2026 Dennis Unger, Modding Forge.

Skyrim Runtime Swapper is licensed under the [GNU General Public License version 3](LICENSE).
