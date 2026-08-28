# Changelog

All notable changes to Skyrim Runtime Swapper are documented in this file.

## 1.0.0 - 2026-08-28

### Added

- Pre-SKSE bootstrap through a complete Windows `version.dll` proxy.
- Transactional runtime swapping with verified backups and automatic rollback.
- Cached runtime activation and process-handle based session restoration.
- Target-runtime pass-through without restoring game files that were not swapped.
- Safe `ContentCatalog.txt` handling for older runtimes.
- Target-specific builds for Skyrim 1.6.1170, 1.6.640, and 1.5.97 from Steam runtime 1.7.104.
- Embedded Windows metadata for every generated DLL and EXE.
- Automated multi-target builds, tests, package validation, and archive generation.
