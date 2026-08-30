#pragma once

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

}  // namespace runtime_swapper
