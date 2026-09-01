# Changelog

All notable changes to Skyrim Runtime Swapper are documented in this file.

## 1.2.1 - 2026-09-02

- Restored verified target-runtime pass-through for installations that were
  already downgraded, including persistent states left by version 1.1.0.
- Kept Creation Club and ContentCatalog handling active during pass-through
  without incorrectly requiring the live runtime to be Skyrim 1.7.104.
- Kept diagnostics in one log file while retaining only the latest 30 primary
  starts and their associated watcher output.
- Added unambiguous run, session, and parent IDs to every diagnostic line and
  removed pre-v2 logs automatically when the new log format starts.

## 1.2.0 - 2026-09-01

- Added portable storage support for Windows, Linux, Wine, and Proton. Trusted
  internal NTFS, ext4, XFS, and Btrfs volumes use automatic sessions, while
  external, removable, exFAT, ntfs-3g, and uncertain local filesystems can use
  a recoverable persistent downgrade with an independent durable vault.
- Introduced a shared crash- and power-loss-safe recovery lifecycle with
  content-addressed backups, durable journals, conflict preservation, stable
  volume identities, idempotent recovery, and automatic cleanup after a fully
  verified restore.
- Separated recovery data, reusable target cache, transaction workspace, and
  coordination locks from the deployed Skyrim directory. Safe same-volume
  Steam-library storage is preferred for faster switching.
- Bound Windows and POSIX mutations to verified filesystem objects, closing
  path-exchange races and preventing writes through unsafe links, junctions,
  mount changes, or replaced volumes.
- Added support for safe symlinks, hard links, reparse points, Windows volume
  mounts, POSIX bind mounts, Bazzite home aliases, Amethyst `Data_Core` and VFS
  layouts, and byte-identical renamed SKSE launcher entries.
- Moved Creation Club and ContentCatalog handling into the shared recovery
  lifecycle, including migration of older layouts and safe reuse of verified
  transaction storage instead of unrelated XDG state paths.
- Added a hash-pinned native Linux sidecar for Wine and Proton, built against
  the Ubuntu 22.04 ABI and reverified before launch. Executable permissions are
  preserved or safely repaired when a mod manager strips them.
- Improved launch performance through verified-file reuse, batched journal and
  directory synchronization, reflink or copy acceleration, a disposable target
  cache, and a safe deterministic retry for FUSE-backed HDiffPatch output.
- Added detailed failure diagnostics for storage decisions, recovery phases,
  file identities, links, hashes, patch I/O, native errors, and timings. Logs
  now live under `Documents\My Games\Skyrim Special Edition\SKSE`.
- Safely retired verified v1.0.0 and v1.1.0 backups, caches, markers, and stale
  locks on first launch while preserving active, unknown, or unverifiable state.
- Added filesystem and fault-injection matrices, adversarial race tests,
  ASan/UBSan coverage, HDiffPatch fuzzing, reproducible builds, dependency-pin
  checks, and explicit ELF and PE hardening verification.
- Release packages contain only the runtime payload, Proton Experimental
  instructions, and Vortex metadata. Every archive is verified and listed in
  `SHA256SUMS.txt`.

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
