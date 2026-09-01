# Changelog

All notable changes to Skyrim Runtime Swapper are documented in this file.

## 1.2.0-rc17 - 2026-09-01

- Moves Creation Club recovery data and temporary holds outside Skyrim, binds
  link layout and volume identity, and retains migration from older layouts.
- Unifies Windows and POSIX ContentCatalog recovery under one v2 state machine
  while retaining both v1 formats for recovery.
- Separates operational paths from Windows, Wine, and POSIX presentation and
  adds normalized, safely quoted diagnostics with phase timings.
- Builds the Linux sidecar against the Ubuntu 22.04 ABI, verifies Linux and
  Windows binary hardening, and runs CI safety tests on a stable ext4 volume.
- Adds verified release archives, SHA-256 sums, complete third-party notices,
  and the Proton Experimental instructions to every package.

## 1.2.0-rc16 - 2026-09-01

- Separates runtime activation, recovery, transaction support, and workspace
  policy into focused core modules without changing recovery invariants.
- Separates Windows and POSIX storage probing from their atomic file
  primitives, with typed internal interfaces shared by all callers.
- Retains RC15 bind-mount recovery support and the complete RC10 hardening,
  fault-injection, sanitizer, and filesystem test coverage.
- Removes duplicated transaction decisions and keeps comments limited to
  non-obvious safety constraints and compatibility behavior.

## 1.2.0-rc15 - 2026-09-01

- Detects POSIX rename namespaces by mount ID, including bind mounts that share
  a filesystem device but still reject cross-mount renames with `EXDEV`.
- Stages only disposable patch and rollback files beside redirected effective
  targets when Amethyst exposes `Data_Core` through a separate mount. The
  durable journal and recovery vault remain outside the deployed game tree.
- Reuses the existing reverse patches and verified vault fallback for restore;
  deterministic mount-local files are rediscovered after crashes and unknown
  content at those names is never deleted.
- Moves mount-local transaction path and cleanup policy into a focused core
  module and adds a real Linux bind-mount regression gate.

## 1.2.0-rc14 - 2026-09-01

- Allows read-only source and patch inputs deployed as final symbolic links or
  reparse points while retaining handle-bound identity and hash validation.
- Revalidates that every linked input still resolves to the same regular-file
  object after HDiffPatch; linked output paths remain forbidden and the staged
  runtime must still match the exact expected target SHA-256 before commit.
- Adds a native regression test for symlinked patch inputs and identifies
  source-input and patch-input open failures separately in diagnostics.

## 1.2.0-rc13 - 2026-09-01

- Adds failure-only SHA-256 diagnostics for the expected source, expected
  target, actual patch input, patch asset, staged output, and live output by
  reusing the hashes already calculated by the safety checks.
- Reports stored and resolved targets for symbolic links and reparse points.
  Hard-link failures report their filesystem object identity and link count;
  hard links have no distinguished source name.
- Identifies the exact managed and staged paths involved so Linux, Proton, and
  mod-manager deployment failures can be diagnosed without asking users to
  calculate checksums manually.

## 1.2.0-rc12 - 2026-09-01

- Moved disposable staging, recovery locators, and target-volume markers from
  the Skyrim directory to a private, same-volume Steam-library workspace.
- Uses a unique directory for every transaction and binds interrupted recovery
  to that directory through the durable journal, including clean-target
  restores that have no earlier activation journal.
- Keeps RC5 through RC11 recovery readable while preventing Amethyst VFS and
  physical deployments from capturing new SRS transaction files into
  `Root_Folder` and redeploying a stale Dawnguard staging output.
- Removed the Skyrim-local fixed-runtime marker and added regression coverage
  for stale `staged/0`, `Data_Core` links, rebuilt managed links, persistent
  markers, cleanup, and protocol-v5 native sidecar path transport.

## 1.2.0-rc11 - 2026-09-01

- Fixed fresh Linux and Proton launches where the coordination lock created a
  volume-local storage root with the desktop umask and vault preparation then
  rejected that same root instead of safely restricting SRS-owned directories.
- Replaced oversized runtime-layout journal names with compact fingerprints and
  added safe recovery for recognized RC10 transactions after Amethyst rebuilds
  final managed links, including vanilla files exposed from `Data_Core`.
- Treats equal-sized but corrupted target-cache objects as disposable: their
  SRS-created staging candidates are removed and rebuilt from verified patches
  during the same launch without touching unknown pre-existing files.
- Restores large live files and verified vault fallbacks through a journaled
  two-rename exchange, avoiding direct NTFS replacement failures and retaining
  a recoverable discard across every power-loss boundary.
- Updated native test transport to protocol v4 and added real-file regression
  coverage for default Linux permissions, cache corruption, renamed SKSE
  launchers, Amethyst link rebinding, restore, and cleanup.

## 1.2.0-rc10 - 2026-08-31

- Bound Windows and POSIX mutation primitives to opened files and directories,
  with structured partial-mutation results and adversarial path-exchange tests.
- Added stable Linux filesystem identities, fail-closed mount-only identities,
  XDG ownership preservation, checked size arithmetic, and random temporary
  names with exclusive creation.
- Reverified the pinned ELF helper immediately before every Wine launch path
  and described the one-shot IPC channel accurately as nonce-bound.
- Passed already-opened source, patch, and output streams into HDiffPatch and
  added recursive dependency-pin verification, ASan/UBSan, adapter fuzzing,
  and reproducible native-build gates.
- Supports Amethyst symlink deployments where its byte-identical launcher copy
  is local to Skyrim but the read-only SKSE loader resolves into the deployment
  store.

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
- Hardened transaction journals, conflict preservation, vault identity validation, locator repair, nonce-bound native sidecar IPC, and crash recovery.
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
