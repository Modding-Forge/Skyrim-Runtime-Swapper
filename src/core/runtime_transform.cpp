#include "internal/runtime_transform.hpp"

#include "internal/file_operations.hpp"

#include <runtime_swapper/hdiff_patch.hpp>
#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/runtime_version.hpp>

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace runtime_swapper::core {
namespace {

enum class FileState { source, target, unknown };

struct WorkItem {
  const PatchPlanEntry* plan{};
  std::filesystem::path live;
  std::filesystem::path patch;
  std::filesystem::path staged;
  std::filesystem::path rollback;
};

[[nodiscard]] std::wstring source_version() { return std::wstring(source_version_label); }
[[nodiscard]] std::wstring target_version() { return std::wstring(target_version_label); }

[[nodiscard]] bool matches_state(const std::filesystem::path& file, bool present,
                                 std::string_view hash) {
  std::error_code error;
  const bool exists = std::filesystem::exists(file, error);
  if (error) return false;
  if (!present) return !exists;
  return std::filesystem::is_regular_file(file, error) && !error && hash_matches(file, hash);
}

[[nodiscard]] FileState inspect_file(const std::filesystem::path& file,
                                     const PatchPlanEntry& plan) {
  if (matches_state(file, plan.source_present, plan.source_sha256)) return FileState::source;
  if (matches_state(file, plan.target_present, plan.target_sha256)) return FileState::target;
  return FileState::unknown;
}

[[nodiscard]] bool restore_transaction_file(const std::filesystem::path& rollback,
                                            const std::filesystem::path& live) {
  if (!std::filesystem::is_regular_file(rollback)) return true;
  std::error_code error;
  std::filesystem::create_directories(live.parent_path(), error);
  if (error) return false;
  return MoveFileExW(rollback.c_str(), live.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

[[nodiscard]] bool recover_stale_transactions(const std::filesystem::path& game_root,
                                              const std::filesystem::path& work_root) {
  std::error_code error;
  const bool work_root_exists = std::filesystem::exists(work_root, error);
  if (error) return false;
  if (!work_root_exists) return true;
  for (std::filesystem::directory_iterator iterator(work_root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_directory(error) || error) continue;
    const auto staging_root = iterator->path();
    if (!staging_root.filename().wstring().starts_with(L"staging-")) continue;

    for (std::size_t index = 0; index < patch_plan.size(); ++index) {
      const auto rollback = staging_root / L"rollback" / std::to_wstring(index);
      const auto live = game_root / patch_plan[index].relative_file;
      if (!restore_transaction_file(rollback, live)) return false;
    }
    std::filesystem::remove_all(staging_root, error);
    if (error) return false;
  }
  return !error;
}

[[nodiscard]] bool commit_file(const WorkItem& item) {
  std::error_code error;
  std::filesystem::create_directories(item.rollback.parent_path(), error);
  if (error) return false;
  if (!MoveFileExW(item.live.c_str(), item.rollback.c_str(), MOVEFILE_WRITE_THROUGH)) return false;
  if (MoveFileExW(item.staged.c_str(), item.live.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  (void)MoveFileExW(item.rollback.c_str(), item.live.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  return false;
}

[[nodiscard]] bool rollback_files(const std::vector<const WorkItem*>& committed) {
  bool success = true;
  for (auto iterator = committed.rbegin(); iterator != committed.rend(); ++iterator) {
    success = restore_transaction_file((*iterator)->rollback, (*iterator)->live) && success;
  }
  return success;
}

class TransactionCleanup {
 public:
  explicit TransactionCleanup(std::filesystem::path root) : root_(std::move(root)) {}
  ~TransactionCleanup() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  TransactionCleanup(const TransactionCleanup&) = delete;
  TransactionCleanup& operator=(const TransactionCleanup&) = delete;

 private:
  std::filesystem::path root_;
};

[[nodiscard]] std::filesystem::path session_marker(const std::filesystem::path& work_root) {
  return work_root / L"target-session.pending";
}

[[nodiscard]] bool write_session_marker(const std::filesystem::path& work_root) {
  std::error_code error;
  std::filesystem::create_directories(work_root, error);
  if (error) return false;

  const auto marker = session_marker(work_root);
  auto temporary = marker;
  temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
  remove_file_if_present(temporary);
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream << "Skyrim 1.6.1170 session pending\n";
    if (!stream) return false;
  }
  if (MoveFileExW(temporary.c_str(), marker.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  remove_file_if_present(temporary);
  return false;
}

[[nodiscard]] bool remove_session_marker(const std::filesystem::path& work_root) {
  std::error_code error;
  const bool removed = std::filesystem::remove(session_marker(work_root), error);
  return !error && (removed || !std::filesystem::exists(session_marker(work_root), error));
}

}  // namespace

DowngradeResult transform_runtime(const std::filesystem::path& game_root,
                                  const std::filesystem::path& patch_root,
                                  bool to_target) {
  try {
    if (!std::filesystem::is_regular_file(game_root / L"SkyrimSE.exe")) {
      return {ExitCode::game_not_found, false, L"SkyrimSE.exe was not found."};
    }

    const auto work_root = game_root / L".skyrim-runtime-swapper";
    if (!recover_stale_transactions(game_root, work_root)) {
      return {ExitCode::restore_failed, false,
              L"An interrupted runtime transaction could not be recovered."};
    }
    if (to_target && std::filesystem::is_regular_file(session_marker(work_root))) {
      const auto recovered = transform_runtime(game_root, patch_root, false);
      if (!recovered.success()) {
        return {recovered.code, false,
                L"A previous target-runtime session could not be restored: " +
                    recovered.message};
      }
    }

    const auto staging_root = work_root / (L"staging-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::create_directories(staging_root, error);
    if (error) {
      return {ExitCode::patch_failed, false, L"The staging directory could not be created."};
    }
    const TransactionCleanup cleanup(staging_root);

    std::vector<WorkItem> work;
    std::uint64_t required_space = 64ULL * 1024ULL * 1024ULL;
    for (std::size_t index = 0; index < patch_plan.size(); ++index) {
      const auto& plan = patch_plan[index];
      const auto live = game_root / plan.relative_file;
      const auto state = inspect_file(live, plan);
      if (state == FileState::unknown) {
        return {ExitCode::source_hash_mismatch, false,
                L"Unknown, modified, or unexpected game file: " + quote_path(live)};
      }
      const auto desired = to_target ? FileState::target : FileState::source;
      if (state == desired) continue;

      const auto patch_name = to_target ? plan.forward_patch : plan.reverse_patch;
      const auto patch_hash =
          to_target ? plan.forward_patch_sha256 : plan.reverse_patch_sha256;
      WorkItem item{
          .plan = &plan,
          .live = live,
          .patch = patch_root / patch_name,
          .staged = staging_root / L"desired" / std::to_wstring(index),
          .rollback = staging_root / L"rollback" / std::to_wstring(index),
      };
      if (!hash_matches(item.patch, patch_hash)) {
        return {ExitCode::patch_files_missing, false,
                L"A required patch is missing or modified for " + quote_path(live)};
      }
      required_space += to_target ? plan.target_size : plan.source_size;
      work.push_back(std::move(item));
    }

    if (work.empty()) {
      if (!to_target && !remove_session_marker(work_root)) {
        return {ExitCode::restore_failed, false,
                L"The completed runtime session marker could not be removed."};
      }
      return {ExitCode::success, false,
              L"Skyrim " + (to_target ? target_version() : source_version()) +
                  L" is already active."};
    }
    if (!has_minimum_free_space(game_root, required_space)) {
      return {ExitCode::insufficient_disk_space, false,
              L"There is not enough free space to stage the verified runtime swap."};
    }

    for (const auto& item : work) {
      const auto desired_hash =
          to_target ? item.plan->target_sha256 : item.plan->source_sha256;
      const auto patched = apply_hdiff_patch(item.live, item.patch, item.staged);
      if (!patched.success || !hash_matches(item.staged, desired_hash)) {
        return {ExitCode::patch_failed, false,
                patched.success
                    ? L"A staged runtime file failed verification: " + quote_path(item.live)
                    : patched.error + L" File: " + quote_path(item.live)};
      }
    }

    std::vector<const WorkItem*> committed;
    for (const auto& item : work) {
      if (!commit_file(item)) {
        const bool rolled_back = rollback_files(committed);
        return {ExitCode::commit_failed, !rolled_back,
                rolled_back ? L"The runtime swap failed and was rolled back."
                            : L"The runtime swap failed and automatic rollback was incomplete."};
      }
      committed.push_back(&item);
    }

    for (const auto& item : work) {
      const auto expected_hash =
          to_target ? item.plan->target_sha256 : item.plan->source_sha256;
      if (!hash_matches(item.live, expected_hash)) {
        const bool rolled_back = rollback_files(committed);
        return {ExitCode::commit_failed, !rolled_back,
                L"Final runtime verification failed; rollback was attempted."};
      }
    }
    if (to_target && !write_session_marker(work_root)) {
      const bool rolled_back = rollback_files(committed);
      return {ExitCode::commit_failed, !rolled_back,
              rolled_back ? L"The runtime session could not be recorded and was rolled back."
                          : L"The runtime session could not be recorded and rollback failed."};
    }
    if (!to_target && !remove_session_marker(work_root)) {
      return {ExitCode::restore_failed, true,
              L"The source runtime was restored, but its session marker could not be removed."};
    }
    return {ExitCode::success, true,
            L"Skyrim was switched from " + (to_target ? source_version() : target_version()) +
                L" to " + (to_target ? target_version() : source_version()) + L"."};
  } catch (const std::exception&) {
    return {ExitCode::internal_error, false,
            L"An unexpected internal error occurred during the runtime swap."};
  }
}

}  // namespace runtime_swapper::core
