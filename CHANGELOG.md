# Changelog

All notable changes to Skyrim Runtime Swapper are documented in this file.

## 1.2.0-rc3 - 2026-08-31

- Added portable storage classification and automatically selected recovery vaults for Windows, Linux, Wine, and Proton.
- Added recoverable persistent downgrades for external, exFAT, ntfs-3g, and otherwise non-automatic local filesystems.
- Hardened transaction journals, conflict preservation, vault identity validation, locator repair, native sidecar authentication, and crash recovery.
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
