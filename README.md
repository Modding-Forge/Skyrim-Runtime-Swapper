# Skyrim Runtime Swapper

Skyrim Runtime Swapper lets a Skyrim mod collection built for an older runtime start through the unmodified `skse64_loader.exe`. The original Steam runtime is restored automatically after every game session.

Current release: `1.0.0`.

## Profiles

Both profiles are available for Skyrim `1.6.1170` and `1.5.97`. They switch an installed `1.7.104` runtime before launch and restore `1.7.104` after the game exits.

- Best of Both Worlds patches `SkyrimSE.exe`, `SkyrimSELauncher.exe`, and `Skyrim - Shaders.bsa`.
- Best of All Worlds also patches `Skyrim - Interface.bsa` and the five official master files.
- The 1.5.97 profiles additionally patch `steam_api64.dll` and create `binkw64.dll` for the session. Their runtime is 1.5.97, while runtime-facing data uses the proven 1.6.1170 AE-compatible baseline.
- The 1.5.97 Best of All Worlds profile also quarantines every present official `cc*` plugin and BSA from the root of `Data` for the game session.

Other source files, stores, and runtimes are rejected without modification.

## Installation

1. Choose the archive matching the runtime required by the collection.
2. Install it as a normal root mod through the mod manager and deploy it.
3. Verify that `version.dll`, `SkyrimRuntimeSwapper.exe`, and `RuntimeSwap\patches` exist in the Skyrim game directory.
4. Launch the unmodified `skse64_loader.exe` selected by the collection.

Do not combine multiple target archives in one installation.

## How it works

The native `version.dll` proxy is loaded by `skse64_loader.exe` before SKSE checks `SkyrimSE.exe`. It forwards all 17 exports provided by the Windows system DLL and starts `SkyrimRuntimeSwapper.exe` only for the Skyrim runtime check.

The helper validates the runtime, patch assets, and game files with SHA-256. HDiffPatch 5.1.3, Zstandard, and XXH128 support are linked statically into the helper. It applies HDIFFW26 patches to staged files and commits the transaction only after every result is verified. A flushed append-only journal records every staging, replacement, commit, and recovery boundary. Unknown, modified, or incomplete files fail closed.

Every SKSE launch checks the current user's `ContentCatalog.txt`, including launches where Skyrim already uses the target runtime. An incompatible catalog is held in a separate same-volume transaction, removed for the game session, and restored afterward.

For the 1.5.97 Best of All Worlds profile, official Creation Club files with the `cc` filename prefix and a `.bsa`, `.esl`, `.esm`, or `.esp` extension are handled separately. Before any file moves, a verified inventory containing each filename, size, and SHA-256 hash is committed under `.skyrim-runtime-swapper\backups\1.7.104\CreationClub`. Each file is then atomically moved there on the same volume. No file data is copied. Skyrim starts only after every quarantined file matches the recorded hash. The watcher moves all files back after the session, and startup recovery resolves an interrupted move before any new launch.

Visible Runtime Swapper errors include buttons to copy the Runtime Swapper and newest SKSE logs or open Steam's file verification action.

A detached watcher locates the exact `SkyrimSE.exe` process and waits on its process handle without polling during gameplay. When the process exits, it applies the verified reverse patches and restores the source runtime. If the target runtime was already installed, game binaries remain untouched and only a temporarily removed `ContentCatalog.txt` is restored.

A session barrier remains active until restoration is complete. If SKSE is launched again immediately after Skyrim exits, the new launch waits for the previous watcher before it inspects or swaps any runtime files.

On the first actual downgrade, the helper creates a persistent verified fallback under `.skyrim-runtime-swapper\backups\1.7.104`. It contains only the original 1.7.104 files managed by the selected profile. Best of Both Worlds therefore backs up only its patch set, while Best of All Worlds also includes its additional managed interface and master files. Target-only files are not backed up. Normal restoration still uses temporary originals or reverse patches. The persistent copies are used only when those paths cannot produce a valid 1.7.104 hash.

Backup files are copied atomically and verified before the downgrade begins. The first downgrade therefore requires additional disk space equal to the managed source files. Recovery runs before the executable and runtime checks and resolves an interrupted or mixed transaction to Skyrim 1.7.104.

On native Windows, file changes require a local fixed NTFS volume without reparse-point traversal. Under Wine or Proton, the same journal, hash verification, and recovery logic is used in best-effort mode. Host filesystem durability cannot be guaranteed through Wine's Windows API translation, so power-loss guarantees apply only to the native Windows NTFS backend.

The bootstrap does not use CommonLibSSE, Address Library, or the SKSE plugin API. It runs before regular SKSE plugins and before the Engine Fixes preloader.

## Building

Requirements:

- Windows x64
- Visual Studio with the Desktop development with C++ workload
- CMake 3.25 or newer
- Git with recursive submodule support

The pipeline consumes verified format 3 asset catalogs. Each catalog is a profile superset. CMake selects the exact files, validates both patch directions, builds and tests separate binaries, and creates four complete archives under `dist/builds/<version>/<build-id>/`.

Without an explicit asset path, the build discovers every catalog under `assets/runtime/*/manifest.json`. The 1.5.97 hybrid catalog can be regenerated from verified local Steam depots with `uv run python tools/create-1.5.97-assets.py`. HDiffPatch5 is pinned as a recursive Git submodule under `third_party/HDiffPatch5`.

Explicit asset paths can be supplied when needed:

```text
build.bat Release D:\Assets\manifest.json
```

The build fails if the asset format, algorithm, HDiffPatch version, runtime pair, required profile files, or patch hashes do not match. Bethesda game files are never included.

## Source layout

- `src/app` owns command-line parsing, user-facing diagnostics, ContentCatalog handling, and the game-session watcher.
- `src/core` owns hashing, patch application, validation, and transactional runtime file operations.
- `src/proxy` contains the minimal Windows `version.dll` proxy and bootstrap.
- `src/include/runtime_swapper` contains the core interfaces shared by executables and tests.
- `src/tools` and `src/tests` contain build-time validation and focused native tests.

The proxy only starts the application layer. The application coordinates sessions through the core interfaces, while the core remains independent of dialogs and process-launch behavior.

## License

Copyright (c) 2026 Dennis Unger, Modding Forge.

Skyrim Runtime Swapper is licensed under the [GNU General Public License version 3](LICENSE).
