#pragma once

#include <string_view>

namespace runtime_swapper {

enum class ExitCode : int {
  success = 0,
  invalid_arguments = 10,
  game_not_found = 11,
  version_read_failed = 12,
  unsupported_runtime = 21,
  patch_files_missing = 22,
  source_hash_mismatch = 23,
  insufficient_disk_space = 24,
  patch_failed = 25,
  backup_failed = 26,
  commit_failed = 27,
  another_instance_failed = 28,
  content_catalog_cleanup_failed = 29,
  internal_error = 30,
  watcher_start_failed = 31,
  restore_failed = 32,
  unsupported_filesystem = 33,
  journal_corrupt = 34,
  recovery_failed = 35,
  creation_club_cleanup_failed = 36,
  user_cancelled = 37,
};

[[nodiscard]] constexpr std::string_view exit_code_name(
    ExitCode code) noexcept {
  switch (code) {
    case ExitCode::success: return "success";
    case ExitCode::invalid_arguments: return "invalid-arguments";
    case ExitCode::game_not_found: return "game-not-found";
    case ExitCode::version_read_failed: return "version-read-failed";
    case ExitCode::unsupported_runtime: return "unsupported-runtime";
    case ExitCode::patch_files_missing: return "patch-files-missing";
    case ExitCode::source_hash_mismatch: return "source-hash-mismatch";
    case ExitCode::insufficient_disk_space: return "insufficient-disk-space";
    case ExitCode::patch_failed: return "patch-failed";
    case ExitCode::backup_failed: return "backup-failed";
    case ExitCode::commit_failed: return "commit-failed";
    case ExitCode::another_instance_failed: return "another-instance-failed";
    case ExitCode::content_catalog_cleanup_failed:
      return "content-catalog-cleanup-failed";
    case ExitCode::internal_error: return "internal-error";
    case ExitCode::watcher_start_failed: return "watcher-start-failed";
    case ExitCode::restore_failed: return "restore-failed";
    case ExitCode::unsupported_filesystem: return "unsupported-filesystem";
    case ExitCode::journal_corrupt: return "journal-corrupt";
    case ExitCode::recovery_failed: return "recovery-failed";
    case ExitCode::creation_club_cleanup_failed:
      return "creation-club-cleanup-failed";
    case ExitCode::user_cancelled: return "user-cancelled";
  }
  return "unknown";
}

}  // namespace runtime_swapper
