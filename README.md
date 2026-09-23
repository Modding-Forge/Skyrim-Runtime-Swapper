# Skyrim Runtime Swapper

Run Skyrim mod setups for **1.6.640**, **1.6.1170**, or **1.5.97** through the existing `skse64_loader.exe`. SRS switches the required files before SKSE checks the game version.

Current version: **1.3.2** · [Changelog](CHANGELOG.md) · [User documentation](https://moddingforge.com/docs/skyrim-runtime-swapper) · [Nexus Mods](https://www.nexusmods.com/skyrimspecialedition/mods/189855)

A new downgrade starts from Steam's **Skyrim 1.7.104**. An already-downgraded installation also works if its managed files exactly match the selected package.
Only exact, legitimate Steam files are supported. Modified, unofficial, unlicensed, or pirated game files are not compatible with SRS and cannot be made compatible.

## Choose a package

**Using a Collection?** Let Vortex or your Collection installer install its selected SRS package and dependencies. Use the Collection's SKSE entry; do not add another variant manually. The steps below are for your own mod setup.

Each target runtime has two profiles:

- **Best of Both Worlds:** older runtime with newer 1.7.104 game data.
- **Best of All Worlds:** also switches selected game data and official masters. For 1.5.97, present Creation Club files are kept out of the active game until restoration.

An additional **BoAW-Clean** package for 1.6.1170 combines the downgrade with pinned SSEEdit Quick Auto Clean results for Update, Dawnguard, HearthFires and Dragonborn. It also cleans the supported original `ccvsvsse004-beafarmer.esl` if present; a missing Beafarmer file is skipped. Use this instead of, not alongside, the normal BoAW package. See [clean-package instructions](assets/BOAW-CLEAN-INSTRUCTIONS.txt).

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

Requires Visual Studio's C++ desktop workload on Windows x64, CMake 3.25+, Python 3.11+, and recursive Git submodules. Packaging requires WSL with Python 3.11+, Make and a C/C++ compiler. The Linux helper needs a C++20 toolchain; release helpers target the Ubuntu 22.04 ABI.

```text
git submodule update --init --recursive
build.bat Release
```

The build uses the catalogs in `assets/runtime`, tests each profile for stable releases, and writes seven `.7z` packages to `dist/builds/<version>/<build-id>/`, including the alternative BoAW-Clean 1.6.1170 package. RC builds skip tests by default; `-DSKIP_TESTS=OFF` opts in. For Linux-enabled bundles, supply matching helpers through `NATIVE_SIDECAR_ROOT/<target>/<profile>/SkyrimRuntimeSwapper.Native`. See [the build script](tools/build-all.cmake).

Packaging uses solid LZMA2 with a 64 MiB dictionary, one compression thread, sorted filenames and omitted timestamps. The encoder is built from the pinned vendored LZMA SDK. Windows delegates packaging to the default WSL distribution (override with `-DWSL_DISTRIBUTION=Ubuntu`). Temporary Linux staging preserves the native helper's `0700` mode. Every archive is extracted and all file hashes and native permissions are checked before delivery; `.7z.build.json` receipts record encoder and payload hashes. Identical staged inputs and encoder produce identical archives; this does not claim byte-reproducible compiler outputs. Existing outputs are never overwritten.

Run `python tools/package-7z.py --self-test` to check archive round trips, sidecar permissions, deterministic output and overwrite rejection.

Tests accept `SRS_TEST_ROOT` for an isolated writable fixture directory. On Windows, use a short non-redirected path if the default LocalAppData location is virtualized or exceeds Win32 path limits. Ensure local `CTestCustom.cmake` files do not exclude tests when validating a release.

The full **Storage safety** workflow runs manually through GitHub Actions. It covers the platform matrices, sanitizers, fuzzing, dependency pins, and reproducibility checks. Additional storage fault-injection runners are in [tools/tests](tools/tests).

`SHA256SUMS.txt` accompanies the release packages. Windows binaries are not Authenticode-signed. Packages contain binary patches, not Bethesda game files.


## Tag builds

Pushing a version tag such as `v1.3.2-rc1` or `v1.3.2` starts the
**Tag release builds** workflow. The tag must match `vcpkg.json` and the CMake
release version. Commit the workflow and all required patch assets before tagging.
Manual workflow runs build and test the selected ref but never upload to Nexus.

The workflow builds all seven Linux-enabled packages, checks the Ubuntu 22.04
sidecar ABI and binary hardening, and runs the Windows tests even for RC tags.
Windows stages verified payloads using `-DSTAGE_ONLY=ON`; Linux creates and
round-trip verifies the archives with native execute permissions preserved.

Download `SRS-v<version>-packages` from the completed workflow's artifacts for
the seven `.7z` files and `SHA256SUMS.txt` (90-day retention). This does not
publish a GitHub Release or sign the binaries. The separate manual Storage
safety workflow remains necessary for the extended Linux/Wine/fuzzing matrix.


### Nexus uploads

Stable tags can upload the seven verified archives to the existing Nexus file
IDs configured in `tag-builds.yml` (mod API ID `7318624462239`). RC tags never
upload. Nexus file versions include the `v` prefix (for example, `v1.3.2`).
The official Nexus upload action is pinned to a reviewed commit.

Before enabling uploads:

1. Create the GitHub environment `nexus-production`, restrict it to release tags,
   and configure required reviewers for manual publication approval. An environment
   name in YAML alone does not enable approval protection.
2. Add `NEXUSMODS_API_KEY` as an environment secret, not to source control.
3. Review the seven file mappings and the `main` category used for new versions.
4. Set the repository Actions variable `NEXUS_UPLOAD_ENABLED` to `true`.

Uploads run after all packages pass verification. Existing versions are not
archived, the mod's overall version and changelog are not changed, and mod-manager
download preferences are not explicitly overridden. Each successful job records
the new Nexus version ID in its summary. Uploads are not atomic across seven
files: after a partial failure, check Nexus before rerunning failed jobs. Do not
rerun successful upload jobs; the upstream action can create duplicate versions.

## License

Copyright (c) 2026 Dennis Unger, Modding Forge. Licensed under [GPLv3](LICENSE).
