# Skyrim Runtime Swapper

Run Skyrim mod setups for **1.6.1170** or **1.5.97** through the existing `skse64_loader.exe`. SRS switches the required files before SKSE checks the game version.

Current release: **1.2.1** · [Changelog](CHANGELOG.md) · [User documentation](https://moddingforge.com/docs/skyrim-runtime-swapper) · [Nexus Mods](https://www.nexusmods.com/skyrimspecialedition/mods/189855)

A new downgrade starts from Steam's **Skyrim 1.7.104**. An already-downgraded installation also works if its managed files exactly match the selected package.

## Choose a package

**Using a Collection?** Let Vortex or your Collection installer install its selected SRS package and dependencies. Use the Collection's SKSE entry; do not add another variant manually. The steps below are for your own mod setup.

Both target runtimes have two profiles:

- **Best of Both Worlds:** older runtime with newer 1.7.104 game data.
- **Best of All Worlds:** also switches selected game data and official masters. For 1.5.97, present Creation Club files are kept out of the active game until restoration.

SKSE and its plugins must match the **target runtime**. Regular plugins must match the chosen data profile. Install only one SRS package.

See [profile differences and managed files](docs/modules/ROOT/pages/profiles.adoc).

## Installation

1. Download the target and profile required by your setup.
2. Install and deploy it as a **root mod**, not a Data-only mod. In Vortex, the included JSON selects **Engine Injector** automatically; leave that setting unchanged.
3. Check that `version.dll`, `SkyrimRuntimeSwapper.exe`, `SkyrimRuntimeSwapper.Native`, and `RuntimeSwap/patches` are beside `SkyrimSE.exe`.
4. Install matching SKSE and launch `skse64_loader.exe` normally.

On Linux, the Windows application starts the native helper automatically; do not run `.Native` yourself. It requires x86-64 Linux with glibc 2.35 or newer.

**Proton Experimental:** if SRS does not load, follow [the included launch instructions](assets/PROTON-EXPERIMENTAL-INSTRUCTIONS.txt).

## Automatic or persistent

On supported internal NTFS, ext4, XFS, or Btrfs storage, SRS normally restores 1.7.104 after Skyrim closes. External, removable, exFAT, and some other local storage require a persistent downgrade with a separate durable recovery vault. Unsupported or unverifiable storage is blocked.

Open `SkyrimRuntimeSwapper.exe` to keep the target active or restore 1.7.104. **Persistent mode does not restore automatically.** An already-installed verified target is not upgraded merely because a game session ends.

[Storage modes](docs/modules/ROOT/pages/storage_modes.adoc) · [Manual control panel](docs/modules/ROOT/pages/manual_control.adoc)

## How it works

`version.dll` starts SRS before SKSE's version check. SRS verifies files and patches with SHA-256, backs up originals, and checks patched output before replacing game files. On Linux, the native helper performs those operations.

A watcher restores files changed for an automatic session. Interrupted transactions are recovered before another launch. Present Creation Club and ContentCatalog files are handled when required, including when the runtime is already downgraded.

[Recovery and backup locations](docs/modules/ROOT/pages/safety_recovery.adoc)

## Troubleshooting

Use **Copy logs** in the error dialog. The SRS log is stored alongside SKSE's logs:

```text
Documents/My Games/Skyrim Special Edition/SKSE/SkyrimRuntimeSwapper.log
```

Under Wine or Proton, use the Documents location in the prefix running SKSE. Keep recovery data intact while troubleshooting.

[Troubleshooting guide](docs/modules/ROOT/pages/troubleshooting.adoc) · [Discord help](https://discord.gg/pqEHdWDf8z) · [Report a bug](https://github.com/Modding-Forge/Skyrim-Runtime-Swapper/issues)

## Building

Requires Visual Studio's C++ desktop workload on Windows x64, CMake 3.25+, Python 3, and recursive Git submodules. The Linux helper needs a C++20 toolchain; release helpers target the Ubuntu 22.04 ABI.

```text
git submodule update --init --recursive
build.bat Release
```

The build uses the catalogs in `assets/runtime`, tests each profile, and writes four packages to `dist/builds/<version>/<build-id>/`. For Linux-enabled bundles, supply matching helpers through `NATIVE_SIDECAR_ROOT/<target>/<profile>/SkyrimRuntimeSwapper.Native`. See [the build script](tools/build-all.cmake).

The full **Storage safety** workflow runs manually through GitHub Actions. It covers the platform matrices, sanitizers, fuzzing, dependency pins, and reproducibility checks. Additional storage fault-injection runners are in [tools/tests](tools/tests).

`SHA256SUMS.txt` accompanies the release packages. Windows binaries are not Authenticode-signed. Packages contain binary patches, not Bethesda game files.

## License

Copyright (c) 2026 Dennis Unger, Modding Forge. Licensed under [GPLv3](LICENSE).
