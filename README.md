# Skyrim Runtime Swapper

Skyrim Runtime Swapper lets a Skyrim mod collection built for an older runtime start through the unmodified `skse64_loader.exe`. On trusted internal filesystems it restores the original Steam runtime automatically after every game session. On external, removable, exFAT, or otherwise uncertain local storage it offers a recoverable persistent downgrade instead.

Current release: `1.2.0-rc6`.

## Profiles

Both profiles are available for Skyrim `1.6.1170` and `1.5.97`. They switch an installed `1.7.104` runtime before launch. Automatic mode restores `1.7.104` after the game exits; persistent mode keeps the target active until the user restores it through the GUI.

| Patcher type | Target game version: `1.6.1170` | Target game version: `1.5.97` |
| --- | --- | --- |
| **Best of Both Worlds** | Binary-patches `SkyrimSE.exe`, `SkyrimSELauncher.exe`, and `Data\Skyrim - Shaders.bsa` to their 1.6.1170 versions. | Binary-patches `SkyrimSE.exe`, `SkyrimSELauncher.exe`, `steam_api64.dll`, and `Data\Skyrim - Shaders.bsa`. Creates the target-only `binkw64.dll`. The executable runtime is 1.5.97, while runtime-facing data uses the proven 1.6.1170 AE-compatible baseline. |
| **Best of All Worlds** | Applies all Best of Both Worlds changes. Also binary-patches `Data\Skyrim - Interface.bsa`, `Skyrim.esm`, `Update.esm`, `Dawnguard.esm`, `HearthFires.esm`, and `Dragonborn.esm`. | Applies all Best of Both Worlds changes and the additional interface and master-file patches. Before launch, every present `cc*.bsa`, `cc*.esl`, `cc*.esm`, and `cc*.esp` in the root of `Data` is hash-verified and moved into the fallback backup. These files are restored after the game session. |

Other source files, stores, and runtimes are rejected without modification.

## Installation

1. Choose the archive matching the runtime required by the collection.
2. Install it as a normal root mod through the mod manager and deploy it.
3. Verify that `version.dll`, `SkyrimRuntimeSwapper.exe`, and `RuntimeSwap\patches` exist in the Skyrim game directory. Wine and Proton packages must also contain the build-specific, hash-pinned `SkyrimRuntimeSwapper.Native` ELF helper.
4. Launch the unmodified `skse64_loader.exe` selected by the collection.

Do not combine multiple target archives in one installation.

`SkyrimRuntimeSwapper.exe` can also be opened directly from the Skyrim game directory. Its manual control panel uses the same persistent transaction as the SKSE start dialog. It can keep the packaged target active for external tools or restore Skyrim 1.7.104 together with all persistently handled Creation Club and ContentCatalog files. The recovery vault is always selected by the program. There is no path field, browse button, configuration value, or command-line override.

## How it works

The native `version.dll` proxy is loaded by `skse64_loader.exe` before SKSE checks `SkyrimSE.exe`. It forwards all 17 exports provided by the Windows system DLL and starts `SkyrimRuntimeSwapper.exe` only for the Skyrim runtime check.

The helper validates the runtime, patch assets, and game files with SHA-256. HDiffPatch 5.1.3, Zstandard, and XXH128 support are linked statically into the helper. It applies HDIFFW26 patches to staged files and commits the transaction only after every result is verified. A flushed append-only journal with sequence numbers, record lengths, and CRCs records every staging, replacement, commit, and recovery boundary. Torn tails are repaired before the next append, damaged committed records fail closed, and recovery is idempotent.

Every SKSE launch checks the current user's `ContentCatalog.txt`, including launches where Skyrim already uses the target runtime. Its own volume is classified independently. A catalog on storage without automatic durability is committed to the recovery vault and remains persistent until the GUI restore. Conflicting regenerated catalogs are preserved before the original is restored.

For the 1.5.97 Best of All Worlds profile, official Creation Club files with the `cc` filename prefix and a `.bsa`, `.esl`, `.esm`, or `.esp` extension are handled separately. Before a persistent move, every original and the checksummed inventory are committed to the independent vault. The same-volume quarantine remains an optimization, not the only recovery source. Unicode names are stored portably, Windows case collisions are rejected, and conflicting live files are preserved before recovery.

Visible Runtime Swapper errors include buttons to copy the Runtime Swapper and newest SKSE logs or open Steam's file verification action.

A detached watcher locates the exact `SkyrimSE.exe` process and waits on its process handle without polling during gameplay. When the process exits, it applies the verified reverse patches and restores the source runtime. If the target runtime was already installed, game binaries remain untouched and only a temporarily removed `ContentCatalog.txt` is restored.

A session barrier remains active until restoration is complete. If SKSE is launched again immediately after Skyrim exits, the new launch waits for the previous watcher before it inspects or swaps any runtime files.

Originals are content-addressed under `objects/<sha256>` in an automatically selected per-installation vault. Windows uses Local AppData. Linux and Proton use `$XDG_STATE_HOME`, or `$HOME/.local/state` when it is unset. The vault must be local, owned by the current user, free of symlink or reparse traversal, permission-restricted, large enough for the objects plus reserve, and on a storage backend with native file and directory synchronization.

Internal NTFS on Windows and internal ext4, XFS, or Btrfs on Linux use automatic per-session restoration. External or removable volumes and exFAT use persistent-only mode with a vault on another durable volume. Unknown but stable local storage requires an explicit warning confirmation. Network storage, unsafe paths, missing recovery storage, missing native helpers, and unrecognized source files are hard blocked. A hard block exposes no downgrade action.

Under Wine and Proton the Windows process translates paths and coordinates a native ELF sidecar through a one-shot, nonce-authenticated, length-prefixed exchange in a randomly named private directory. The native helper restricts and validates the directory, rejects links and pre-existing responses, and publishes its response atomically. The ELF SHA-256 is embedded into the Windows binary and verified before launch. Only the native helper mutates managed files, so Linux filesystem synchronization and rename semantics are used directly.

The vault intent is synchronized before the game volume changes. Each live result is hashed again after replacement. Persistent markers are committed in the vault first and then in the game directory. If the active vault disappears or another device appears at a reused path, the transaction remains pending and launch is blocked instead of silently selecting a replacement vault.

The bootstrap does not use CommonLibSSE, Address Library, or the SKSE plugin API. It runs before regular SKSE plugins and before the Engine Fixes preloader.

## Building

Requirements:

- Windows x64 with Visual Studio and the Desktop development with C++ workload for the proxy and GUI
- A Linux C++20 toolchain for the native ELF sidecar
- CMake 3.25 or newer
- Python 3 for the sidecar protocol test
- Git with recursive submodule support

The pipeline consumes verified format 3 asset catalogs. Each catalog is a profile superset. CMake selects the exact files, validates both patch directions, builds and tests separate binaries, and creates four complete archives under `dist/builds/<version>/<build-id>/`.

Without an explicit asset path, the build discovers every catalog under `assets/runtime/*/manifest.json`. The 1.5.97 hybrid catalog can be regenerated from verified local Steam depots with `uv run python tools/create-1.5.97-assets.py`. HDiffPatch5 is pinned as a recursive Git submodule under `third_party/HDiffPatch5`.

Explicit asset paths can be supplied when needed:

```text
build.bat Release D:\Assets\manifest.json
```

The build fails if the asset format, algorithm, HDiffPatch version, runtime pair, required profile files, or patch hashes do not match. Bethesda game files are never included.

The normal CTest suite covers journal migration, torn records, risk acceptance, vault corruption, unknown-file preservation, Unicode names, ContentCatalog conflicts, and transaction cut points. Additional guarded release-gate runners live under `tools/tests` for Windows VHDX classification and detach recovery, Linux loop filesystems, persistent exFAT and ntfs-3g activation plus restore, `dm-log-writes` replay, `dm-flakey` failures, WSL shutdowns, sidecar protocol rejection, and full runtime crash recovery with a locally supplied clean Skyrim fixture. The destructive storage runners create and validate isolated temporary images only, require explicit administrator or root execution, and never select a physical disk.

## Source layout

- `src/app` owns user-facing diagnostics, the shared session and persistent operations, ContentCatalog and Creation Club handling, and the game-session watcher.
- `src/core` owns hashing, automatic vault resolution, storage classification, journals, patch application, validation, and recoverable runtime transactions.
- `src/sidecar` contains the native Linux and Proton transaction entry point.
- `src/proxy` contains the minimal Windows `version.dll` proxy and bootstrap.
- `src/include/runtime_swapper` contains the core interfaces shared by executables and tests.
- `src/tools` and `src/tests` contain build-time validation and focused native tests.

The proxy only starts the application layer. The application coordinates sessions through the core interfaces, while the core remains independent of dialogs and process-launch behavior.

## License

Copyright (c) 2026 Dennis Unger, Modding Forge.

Skyrim Runtime Swapper is licensed under the [GNU General Public License version 3](LICENSE).
