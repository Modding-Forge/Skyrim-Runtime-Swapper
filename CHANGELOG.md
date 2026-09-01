# Changelog

All notable changes to Skyrim Runtime Swapper are documented in this file.

## 1.2.0 - 2026-09-01

- Safely retires verified v1.0.0 and v1.1.0 in-game recovery storage on the
  first v1.2.0 launch, including fallback backups, runtime caches, staging
  directories, completion markers, and stale transaction lock files. Unknown,
  linked, active, or unverifiable legacy state is preserved and blocks cleanup.
- Writes diagnostics beside the other Skyrim mod logs under
  `Documents\My Games\Skyrim Special Edition\SKSE`. The Copy Logs action can
  still read the former AppData log when no new-location log exists.

- Added portable storage classification and automatically selected recovery
  storage for Windows, Linux, Wine, and Proton. Trusted internal NTFS, ext4,
  XFS, and Btrfs volumes support automatic sessions; external, removable,
  exFAT, ntfs-3g, and uncertain local filesystems use recoverable persistent
  downgrades when a durable independent vault is available.
- Added a shared recovery lifecycle for automatic, persistent, interrupted, and
  cleanup-pending transactions. Verified recovery data is removed after a
  complete restore, while incomplete or conflicting states remain recoverable.
- Separated the recovery vault, reusable target cache, transaction workspace,
  and coordination lock into typed locations outside the deployed Skyrim tree.
  Same-volume Steam-library storage is preferred when it is safe.
- Added content-addressed recovery objects, append-only durable journals,
  conflict preservation, stable volume identities, idempotent cleanup, and
  fail-closed recovery after crashes or power loss.
- Bound Windows and POSIX mutations to opened filesystem objects and verified
  identities, closing path-exchange races between validation and commit.
  Temporary names use secure randomness and all size arithmetic is checked.
- Added support for safe final symlinks, reparse points, hard links, Windows
  volume mounts, POSIX bind mounts, Bazzite home aliases, and Amethyst
  `Data_Core` layouts without allowing writes outside the resolved installation.
- Recognizes a byte-identical `SkyrimSELauncher.exe` copy used as an SKSE entry
  point, records its runtime layout, and leaves the launcher alias untouched.
- Keeps disposable patch and rollback staging in the correct rename namespace,
  including Amethyst VFS and bind-mount layouts, while durable recovery remains
  outside the mod-manager deployment tree.
- Stores Creation Club originals and inventory in the Recovery Vault before
  mutation, keeps temporary holds in the external transaction workspace, and
  binds hashes, link layout, file identity, and volume identity. Older layouts
  remain recoverable and migrate through the shared lifecycle.
- Unified Windows and POSIX ContentCatalog recovery under one versioned state
  machine while retaining both legacy formats as migration inputs.
- Added one prepared launch operation with verified-file reuse, batched journal
  intents, bounded directory synchronization, reflink or copy acceleration, and
  a disposable verified target-runtime cache.
- Added structured storage and mutation results plus failure-only diagnostics
  for hashes, link targets, file identities, storage paths, backend decisions,
  and recovery phase timings without adding redundant full-file hash passes.
- Uses the same Core transaction logic through a hash-pinned native ELF sidecar
  under Wine and Proton. Release sidecars target the Ubuntu 22.04 ABI and are
  reverified immediately before every launch attempt. Archives preserve the
  executable mode, while Proton can synchronously repair and verify it when a
  mod manager strips the permission during extraction.
- Keeps trusted volume-local Steam storage independent of an unrelated XDG
  state directory, so a safe internal ext4 installation is not blocked by the
  ownership or permissions of a state path that is not used by the operation.
- Added recursive dependency-pin checks, ASan and UBSan coverage, HDiffPatch
  adapter fuzzing, adversarial race and fault-injection tests, stable-volume CI,
  filesystem matrices, reproducible native builds, and explicit ELF and PE
  hardening verification.
- Release packages now include the README, license, complete third-party
  notices, Proton Experimental instructions, verified archive contents, and a
  persistent `SHA256SUMS.txt` for all four profiles.

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
