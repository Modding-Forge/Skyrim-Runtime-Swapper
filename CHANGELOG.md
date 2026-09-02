# Changelog

All notable changes to Skyrim Runtime Swapper are documented in this file.

## 1.2.2 - 2026-09-02

- Added an alternative BoAW-Clean package for 1.6.1170, combining the downgrade
  with verified SSEEdit 4.1.5f Quick Auto Clean targets for Update, Dawnguard,
  HearthFires and Dragonborn. SSEEdit is not required on the user's machine.
- BoAW-Clean also handles the supported Beafarmer plugin when present and skips
  it when absent. The original file is restored with the source runtime.
- Fixed Windows permission failures caused by unnecessary owner-reassignment
  and directory-delete permissions, while retaining ownership and file checks.
- Fixed repeated BoAW-Clean activation after recovery-vault cleanup by
  re-establishing private storage permissions instead of reusing stale state.
- Improved Linux/Proton support for fresh installations in group-writable
  Steam libraries by selecting validated private user-state storage.
- Preserved conflict files no longer prevent cleanup indefinitely when they
  can be safely verified and archived outside the active recovery vault.
- Added clearer lock, permission and storage-validation diagnostics.
- All five packages now use `.7z` instead of ZIP. Packaging verifies extracted
  file hashes and preserves Linux helper executable permissions.
- Expanded the documentation for package selection, BoAW-Clean, installation,
  Linux storage and recovery troubleshooting.
- Existing inconsistent recovery journals from test builds still fail closed;
  this release does not automatically discard or repair them. The reported
  RC5 installation completed two verified round trips after a manual state
  reset with its original Steam files restored.

## 1.2.2-rc6 - 2026-09-02

- Invalidate prepared storage after successful recovery-vault deletion. The
  next activation re-establishes private vault permissions and directory
  handles instead of reusing the removed directory's preparation state.
  Fixes BoAW-Clean startup failing with original Beafarmer on Windows.
- On retry, re-protect an inherited vault DACL only through the existing
  owner and exclusive user/SYSTEM checks, allowing the safe RC5 leftover to
  recover without deleting recovery data or relaxing access restrictions.
- Preserve the underlying storage-validation reason when optional-file layout
  detection fails, and stop activation immediately on an invalid layout.

## 1.2.2-rc5 - 2026-09-02

- On Linux/Proton, new installations in group-writable Steam libraries use
  private user-state storage for the coordination lock, recovery vault, cache,
  and transaction workspace. Library permissions are never changed. Existing
  storage pins the selection; ambiguous or unsafe old recovery state blocks
  relocation instead of being silently abandoned.
- Added a separate BoAW-Clean 1.6.1170 package: original 1.7.104 masters are
  patched directly to SSEEdit 4.1.5f Quick Auto Clean outputs. Optional
  Beafarmer is skipped only when initially absent; the selection is persisted
  before mutation and retained during recovery. Both patch directions are
  reconstructed and hash-verified when generating the catalog.

## 1.2.2-rc4 - 2026-09-02

- After source-runtime verification, archive preserved conflict files and
  recovery journal snapshots beside the active vault instead of indefinitely
  blocking cleanup and launch. Copies are hash-verified and flushed before
  active recovery data is retired; retries never overwrite conflicting archive
  contents. Archive paths are included in the recovery result.

## 1.2.2-rc3 - 2026-09-02

- Removed the unnecessary FILE_DELETE_CHILD requirement when opening Windows
  directories for file replacement. Normal Modify permissions now suffice for
  the directory handle; individual file DELETE access and identity checks remain
  enforced.

## 1.2.2-rc2 - 2026-09-02

- Removed the unnecessary Windows owner reassignment when restricting SRS
  directory permissions. Ownership is verified before updating the DACL,
  without requesting WRITE_OWNER access. Existing security checks and lock
  failure diagnostics remain in place; Linux behavior is unchanged.

## 1.2.2-rc1 - 2026-09-02

- RC package builds now skip test compilation and execution by default and build
  only the release binaries. Use `-DSKIP_TESTS=OFF` to opt into tests explicitly.
- Added transaction-lock failure diagnostics with the failing operation, lock
  path, process ID, and available native error code and Windows message.
- Distinguished directory creation, ownership, permission, path-safety, exclusive
  open, and lock-file validation failures. Directory preparation now preserves
  native errors instead of reporting a potentially stale Windows error.

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
