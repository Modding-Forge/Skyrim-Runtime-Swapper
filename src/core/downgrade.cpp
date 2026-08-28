#include <runtime_swapper/downgrade.hpp>

#include <runtime_swapper/bspatch.hpp>
#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/sha256.hpp>

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::wstring source_version() { return std::wstring(runtime_swapper::source_version_label); }
std::wstring target_version() { return std::wstring(runtime_swapper::target_version_label); }

enum class FileState { source, target };

struct WorkItem {
  const runtime_swapper::PatchPlanEntry* plan{};
  std::filesystem::path source;
  std::filesystem::path patch;
  std::filesystem::path staged;
  std::filesystem::path backup;
  std::filesystem::path source_cache;
  std::filesystem::path target_cache;
  FileState state{FileState::source};
};

bool hash_matches(const std::filesystem::path& file, std::string_view expected) {
  const auto actual = runtime_swapper::sha256_file(file);
  return actual && *actual == expected;
}

std::wstring quote(const std::filesystem::path& path) { return L"\"" + path.wstring() + L"\""; }

void remove_file_if_present(const std::filesystem::path& file) noexcept {
  std::error_code ignored;
  std::filesystem::remove(file, ignored);
}

bool ensure_free_space(const std::filesystem::path& root) {
  ULARGE_INTEGER available{};
  if (!GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr)) {
    return false;
  }
  constexpr std::uint64_t required = 512ULL * 1024ULL * 1024ULL;
  return available.QuadPart >= required;
}

bool make_verified_backup(const WorkItem& item, DWORD process_id, std::wstring& error) {
  if (std::filesystem::exists(item.backup)) {
    if (hash_matches(item.backup, item.plan->source_sha256)) {
      return true;
    }
    error = L"An existing backup has an unexpected hash: " + quote(item.backup);
    return false;
  }

  std::error_code filesystem_error;
  std::filesystem::create_directories(item.backup.parent_path(), filesystem_error);
  if (filesystem_error) {
    error = L"The backup directory could not be created: " + quote(item.backup.parent_path());
    return false;
  }

  auto temporary = item.backup;
  temporary += L".tmp-" + std::to_wstring(process_id);
  remove_file_if_present(temporary);
  if (!CopyFileW(item.source.c_str(), temporary.c_str(), TRUE) ||
      !hash_matches(temporary, item.plan->source_sha256)) {
    remove_file_if_present(temporary);
    error = L"The backup could not be created safely: " + quote(item.source);
    return false;
  }
  if (!MoveFileExW(temporary.c_str(), item.backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
    remove_file_if_present(temporary);
    error = L"The verified backup could not be finalized: " + quote(item.backup);
    return false;
  }
  return true;
}

bool replace_with_staged(const WorkItem& item) {
  return MoveFileExW(item.staged.c_str(), item.source.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool restore_backup(const WorkItem& item, DWORD process_id) {
  auto rollback = item.source;
  rollback += L".rollback-" + std::to_wstring(process_id);
  remove_file_if_present(rollback);
  if (!CopyFileW(item.backup.c_str(), rollback.c_str(), TRUE) ||
      !hash_matches(rollback, item.plan->source_sha256)) {
    remove_file_if_present(rollback);
    return false;
  }
  if (!MoveFileExW(rollback.c_str(), item.source.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    remove_file_if_present(rollback);
    return false;
  }
  return true;
}

void cleanup_staged(const std::vector<WorkItem>& work) noexcept {
  for (const auto& item : work) {
    remove_file_if_present(item.staged);
  }
}

bool contains_regular_file(const std::filesystem::path& root) {
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->is_regular_file(error)) return true;
    if (error) return true;
  }
  return error.operator bool();
}

void remove_tree_if_file_free(const std::filesystem::path& root) noexcept {
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error || contains_regular_file(root)) return;
  std::filesystem::remove_all(root, error);
}

void cleanup_stale_staging_directories(const std::filesystem::path& work_root) noexcept {
  std::error_code error;
  for (std::filesystem::directory_iterator iterator(work_root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_directory(error) || error) continue;
    const auto name = iterator->path().filename().wstring();
    if (name.starts_with(L"staging-")) remove_tree_if_file_free(iterator->path());
  }
}

struct StagingDirectoryCleanup {
  std::filesystem::path root;
  ~StagingDirectoryCleanup() { remove_tree_if_file_free(root); }
};

bool move_file(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::error_code error;
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error || std::filesystem::exists(destination, error) || error) {
    return false;
  }
  return MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
}

runtime_swapper::DowngradeResult activate_cached_runtime(const std::vector<WorkItem>& work) {
  std::vector<const WorkItem*> activated;
  for (const auto& item : work) {
    if (std::filesystem::exists(item.source_cache) ||
        !hash_matches(item.source, item.plan->source_sha256) ||
        !hash_matches(item.target_cache, item.plan->target_sha256)) {
      return {runtime_swapper::ExitCode::source_hash_mismatch, false,
              L"The runtime cache is incomplete or modified."};
    }

    if (!move_file(item.source, item.source_cache)) {
      goto rollback;
    }
    if (!move_file(item.target_cache, item.source)) {
      move_file(item.source_cache, item.source);
      goto rollback;
    }
    activated.push_back(&item);
  }

  for (const auto& item : work) {
    if (!hash_matches(item.source, item.plan->target_sha256)) {
      goto rollback;
    }
  }
  return {runtime_swapper::ExitCode::success, true,
          L"Skyrim " + target_version() + L" was activated from the local runtime cache."};

rollback:
  for (auto iterator = activated.rbegin(); iterator != activated.rend(); ++iterator) {
    const auto& item = **iterator;
    move_file(item.source, item.target_cache);
    move_file(item.source_cache, item.source);
  }
  return {runtime_swapper::ExitCode::commit_failed, false,
          L"The fast runtime swap failed and was rolled back."};
}

}  // namespace

namespace runtime_swapper {

DowngradeResult downgrade_runtime(const std::filesystem::path& game_root,
                                  const std::filesystem::path& patch_root) {
  try {
    if (!std::filesystem::is_regular_file(game_root / L"SkyrimSE.exe")) {
      return {ExitCode::game_not_found, false, L"SkyrimSE.exe was not found."};
    }
    if (!ensure_free_space(game_root)) {
      return {ExitCode::insufficient_disk_space, false,
              L"At least 512 MiB of free space is required for staging and backups."};
    }

    const DWORD process_id = GetCurrentProcessId();
    const auto work_root = game_root / L".skyrim-runtime-swapper";
    cleanup_stale_staging_directories(work_root);
    const auto staging_root = work_root / (L"staging-" + std::to_wstring(process_id));
    const StagingDirectoryCleanup staging_cleanup{staging_root};
    const auto backup_root = work_root / L"backups" / source_version();
    const auto source_cache_root = work_root / L"versions" / source_version();
    const auto target_cache_root = work_root / L"versions" / target_version();
    std::vector<WorkItem> work;
    work.reserve(patch_plan.size());

    bool needs_changes = false;
    for (const auto& plan : patch_plan) {
      WorkItem item{
          .plan = &plan,
          .source = game_root / plan.relative_file,
          .patch = patch_root / plan.patch_file,
          .staged = staging_root / plan.relative_file,
          .backup = backup_root / plan.relative_file,
          .source_cache = source_cache_root / plan.relative_file,
          .target_cache = target_cache_root / plan.relative_file,
      };

      if (!std::filesystem::is_regular_file(item.source)) {
        return {ExitCode::game_not_found, false,
                L"Required game file is missing: " + quote(item.source)};
      }
      if (hash_matches(item.source, plan.target_sha256)) {
        item.state = FileState::target;
      } else if (hash_matches(item.source, plan.source_sha256)) {
        item.state = FileState::source;
        needs_changes = true;
        if (!std::filesystem::is_regular_file(item.patch)) {
          return {ExitCode::patch_files_missing, false,
                  L"Required patch file is missing: " + quote(item.patch)};
        }
      } else {
        return {ExitCode::source_hash_mismatch, false,
                L"Unknown or modified game file: " + quote(item.source)};
      }
      work.push_back(std::move(item));
    }

    if (!needs_changes) {
      return {ExitCode::success, false,
              L"Skyrim is already on version " + target_version() + L"."};
    }

    const bool all_source = std::all_of(work.begin(), work.end(), [](const WorkItem& item) {
      return item.state == FileState::source;
    });
    if (!all_source) {
      const auto recovered = restore_runtime(game_root);
      if (!recovered.success()) {
        return {recovered.code, false,
                L"An interrupted runtime swap could not be repaired: " +
                    recovered.message};
      }
      return downgrade_runtime(game_root, patch_root);
    }
    const bool target_cache_ready =
        all_source && std::all_of(work.begin(), work.end(), [](const WorkItem& item) {
          return hash_matches(item.target_cache, item.plan->target_sha256);
        });
    if (target_cache_ready) {
      return activate_cached_runtime(work);
    }

    for (const auto& item : work) {
      if (item.state == FileState::target) continue;
      const auto result = apply_bsdiff_patch(item.source, item.patch, item.staged);
      if (!result.success || !hash_matches(item.staged, item.plan->target_sha256)) {
        cleanup_staged(work);
        return {ExitCode::patch_failed, false,
                result.success ? L"The target hash does not match after patching: " + quote(item.source)
                               : result.error + L" File: " + quote(item.source)};
      }
    }

    std::wstring backup_error;
    for (const auto& item : work) {
      if (item.state == FileState::source &&
          !make_verified_backup(item, process_id, backup_error)) {
        cleanup_staged(work);
        return {ExitCode::backup_failed, false, backup_error};
      }
    }

    std::vector<const WorkItem*> committed;
    for (const auto& item : work) {
      if (item.state == FileState::target) continue;
      if (!replace_with_staged(item)) {
        bool rollback_succeeded = true;
        for (auto iterator = committed.rbegin(); iterator != committed.rend(); ++iterator) {
          rollback_succeeded = restore_backup(**iterator, process_id) && rollback_succeeded;
        }
        cleanup_staged(work);
        return {ExitCode::commit_failed, !rollback_succeeded,
                rollback_succeeded
                    ? L"A file could not be replaced; all changes were rolled back."
                    : L"A file could not be replaced and the rollback was incomplete. "
                      L"Restore the files from .skyrim-runtime-swapper\\backups."};
      }
      committed.push_back(&item);
    }

    for (const auto& item : work) {
      if (!hash_matches(item.source, item.plan->target_sha256)) {
        return {ExitCode::commit_failed, true,
                L"Final verification failed: " + quote(item.source)};
      }
    }
    return {ExitCode::success, true, L"Skyrim was successfully downgraded from " + source_version() +
                                         L" to " + target_version() + L"."};
  } catch (const std::exception&) {
    return {ExitCode::internal_error, false,
            L"An unexpected internal error occurred during the downgrade."};
  }
}

DowngradeResult restore_runtime(const std::filesystem::path& game_root) {
  try {
    const DWORD process_id = GetCurrentProcessId();
    const auto work_root = game_root / L".skyrim-runtime-swapper";
    const auto backup_root = work_root / L"backups" / source_version();
    const auto source_cache_root = work_root / L"versions" / source_version();
    const auto target_cache_root = work_root / L"versions" / target_version();
    std::vector<WorkItem> work;
    work.reserve(patch_plan.size());
    for (const auto& plan : patch_plan) {
      WorkItem item{
          .plan = &plan,
          .source = game_root / plan.relative_file,
          .backup = backup_root / plan.relative_file,
          .source_cache = source_cache_root / plan.relative_file,
          .target_cache = target_cache_root / plan.relative_file,
      };
      if (!hash_matches(item.backup, plan.source_sha256)) {
        return {ExitCode::backup_failed, false,
                L"The immutable " + source_version() + L" backup is missing or corrupt: " +
                    quote(item.backup)};
      }
      work.push_back(std::move(item));
    }
    bool changed = false;
    for (const auto& item : work) {
      if (hash_matches(item.source, item.plan->source_sha256)) {
        if (std::filesystem::is_regular_file(item.source_cache)) {
          if (!hash_matches(item.source_cache, item.plan->source_sha256)) {
            return {ExitCode::restore_failed, changed,
                    L"An unexpected " + source_version() +
                        L" cache prevents restoration."};
          }
          remove_file_if_present(item.source_cache);
        }
        continue;
      }

      const bool live_target = hash_matches(item.source, item.plan->target_sha256);
      const bool cached_target = hash_matches(item.target_cache, item.plan->target_sha256);
      if (!live_target && !cached_target) {
        return {ExitCode::restore_failed, changed,
                L"Neither the active file nor the cache contains the expected " +
                    target_version() + L" file: " +
                    quote(item.source)};
      }

      if (live_target) {
        if (cached_target) {
          remove_file_if_present(item.source);
        } else if (!move_file(item.source, item.target_cache)) {
          return {ExitCode::restore_failed, changed,
                  L"An active " + target_version() +
                      L" file could not be moved to the cache."};
        }
      }

      const bool source_from_cache = std::filesystem::is_regular_file(item.source_cache);
      bool source_restored = false;
      if (source_from_cache) {
        source_restored = hash_matches(item.source_cache, item.plan->source_sha256) &&
                          move_file(item.source_cache, item.source);
      } else {
        auto temporary = item.source;
        temporary += L".restore-" + std::to_wstring(process_id);
        remove_file_if_present(temporary);
        source_restored = CopyFileW(item.backup.c_str(), temporary.c_str(), TRUE) &&
                          hash_matches(temporary, item.plan->source_sha256) &&
                          MoveFileExW(temporary.c_str(), item.source.c_str(),
                                      MOVEFILE_WRITE_THROUGH) != FALSE;
        remove_file_if_present(temporary);
      }

      if (!source_restored) {
        return {ExitCode::restore_failed, changed,
                L"Skyrim " + source_version() +
                    L" could not be restored completely."};
      }
      changed = true;
    }

    for (const auto& item : work) {
      if (!hash_matches(item.source, item.plan->source_sha256) ||
          !hash_matches(item.target_cache, item.plan->target_sha256)) {
        return {ExitCode::restore_failed, changed,
                L"Final verification of the restoration swap failed."};
      }
    }
    remove_tree_if_file_free(source_cache_root);
    return {ExitCode::success, changed,
            changed ? L"Skyrim " + source_version() + L" was restored; " +
                          target_version() + L" is available in the fast cache."
                    : L"Skyrim " + source_version() + L" is already restored."};
  } catch (const std::exception&) {
    return {ExitCode::internal_error, false,
            L"An unexpected internal error occurred during restoration."};
  }
}

}  // namespace runtime_swapper
