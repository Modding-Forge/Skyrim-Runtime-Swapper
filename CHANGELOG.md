# Changelog

All notable changes to Skyrim Runtime Swapper are documented in this file.

## 1.0.0 - 2026-08-28

### Added

- Pre-SKSE bootstrap through a complete Windows `version.dll` proxy.
- Bidirectional HDIFFW26 runtime swapping with automatic transaction rollback.
- Process-handle based session restoration without persistent game-file backups or caches.
- Target-runtime pass-through without restoring game files that were not swapped.
- Safe `ContentCatalog.txt` handling for older runtimes.
- Best of Both Worlds and Best of All Worlds profiles for Skyrim 1.7.104 and 1.6.1170.
- Embedded Windows metadata for every generated DLL and EXE.
- Asset-driven CMake builds, tests, patch validation, and complete archive generation.
- Statically linked HDiffPatch 5.1.3 runtime with native Zstandard and XXH128 support.
