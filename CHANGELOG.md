# Changelog

All notable changes to Skyrim Runtime Swapper are documented in this file.

## 1.2.0-rc9 - 2026-08-31

- Recognizes Amethyst's byte-identical `SkyrimSELauncher.exe` copy as an SKSE
  loader entry point without hashing it.
- Records a dedicated runtime layout and leaves the active SKSE launcher alias
  untouched during downgrade, recovery, and source verification.
- Blocks a transaction if the launcher layout changes while it is active and
  adds cross-platform regression tests for alias detection and patch-plan scope.
- Runs launch recovery and activation through one prepared storage operation,
  reusing verified file identities and hashes while the underlying file remains
  unchanged.
- Commits file intents in journal batches and synchronizes each affected
  directory once per safety boundary instead of once per managed file.
- Treats the target cache as disposable performance data, while keeping every
  recovery-vault verification and fail-closed identity check intact.

## 1.2.0-rc8 - 2026-08-31

- Retired orphaned RC5/RC6 recovery locators when the complete source state is independently verified.
- Kept missing-vault recovery strictly blocked for active target runtimes, persistent state, pending ContentCatalog data, or unknown transaction metadata.
- Added Bazzite-style stale-locator integration coverage for both the safe migration and fail-closed target-state cases.

## 1.2.0-rc7 - 2026-08-31

- Added a shared recovery lifecycle for automatic, persistent, interrupted, and cleanup-pending transactions.
- Separated recovery storage, reusable target cache, and coordination locks into distinct typed locations.
- Removed verified recovery vaults, locators, and empty Skyrim transaction metadata before the watcher exits.
- Retained recovery data on incomplete verification, preserved conflicts, or interrupted cleanup and made cleanup idempotent.
- Added safe non-following recovery-tree deletion primitives for Windows and Linux and retained RC5/RC6 manifest compatibility.
- Unified watcher, GUI, and native sidecar restoration through the same complete installation recovery operation.

## 1.2.0-rc6 - 2026-08-31

- Added a verified content-addressed target-runtime cache for automatic sessions, avoiding repeated HDiffPatch work after the first successful switch.
- Added portable clone-or-copy storage operations with Linux copy-on-write reflinks and a durable atomic-copy fallback.
- Removed redundant source-vault and cache hash passes while retaining independent verification of every materialized staged and live file.
- Added per-phase performance diagnostics for recovery, preflight, vault verification, staging, commit, and target-cache hits.
- Added cache-corruption, clone isolation, fallback, and destination-verification coverage.

## 1.2.0-rc5 - 2026-08-31

- Added platform-neutral support for managed file links whose verified targets remain inside the locked Skyrim installation and on the same volume.
- Preserved managed links while atomically switching and restoring their regular-file targets.
- Bound every runtime journal to the complete resolved file layout so retargeted links block recovery before any write.
- Added internal-link, external-target, retargeting, Btrfs transaction, and interrupted-recovery coverage.

## 1.2.0-rc4 - 2026-08-31

- Added portable storage classification and automatically selected recovery vaults for Windows, Linux, Wine, and Proton.
- Added recoverable persistent downgrades for external, exFAT, ntfs-3g, and otherwise non-automatic local filesystems.
- Hardened transaction journals, conflict preservation, vault identity validation, locator repair, native sidecar authentication, and crash recovery.
- Accepted canonical Bazzite and Fedora Atomic home aliases without allowing symlinks inside the recovery-vault hierarchy.
- Added log copying to hard-block dialogs and recorded the complete storage probe reason before showing them.
- Added Windows, Linux, WSL, Wine, filesystem, persistent-mode, fault-injection, and block-corruption test matrices.

## 1.1.0 - 2026-08-29

- Added a native manual control panel for persistent target-runtime switching and explicit 1.7.104 restoration.

## 1.0.0 - 2026-08-28

### Added

- Pre-SKSE bootstrap through a complete Windows `version.dll` proxy.
- Bidirectional HDIFFW26 runtime swapping with automatic transaction rollback.
- Process-handle based session restoration with profile-scoped 1.7.104 fallback backups.
- Target-runtime pass-through without restoring game files that were not swapped.
- Safe `ContentCatalog.txt` handling for older runtimes.
- Best of Both Worlds and Best of All Worlds profiles for Skyrim 1.7.104 to 1.6.1170 and 1.5.97.
- Verified 1.5.97 native runtime patches with a 1.6.1170 AE-compatible data baseline.
- Transactional creation and removal of target-only runtime files such as `binkw64.dll`.
- Embedded Windows metadata for every generated DLL and EXE.
- Asset-driven CMake builds, tests, patch validation, and complete archive generation.
- Statically linked HDiffPatch 5.1.3 runtime with native Zstandard and XXH128 support.
- Durable append-only runtime transactions with recovery to Skyrim 1.7.104 before version checks.
- Same-volume transactional ContentCatalog handling and combined error-log copying.
- Strict native Windows NTFS validation and Wine or Proton best-effort support.
- Atomic, verified first-downgrade backups containing only files managed by the selected profile.
- Journaled same-volume Creation Club quarantine and recovery for the 1.5.97 Best of All Worlds profile.
