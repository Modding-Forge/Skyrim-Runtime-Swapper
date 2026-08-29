#include "internal/runtime_transform.hpp"

#include "internal/file_operations.hpp"
#include "internal/runtime_backup.hpp"
#include "internal/transaction_journal.hpp"

#include <runtime_swapper/hdiff_patch.hpp>
#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace runtime_swapper::core {
namespace {

enum class FileState { source, target, unknown };

struct WorkItem {
  std::size_t index{};
  const PatchPlanEntry* plan{};
  std::filesystem::path live;
  std::filesystem::path patch;
  std::filesystem::path staged;
  std::filesystem::path rollback;
};

[[nodiscard]] std::wstring source_version() { return std::wstring(source_version_label); }
[[nodiscard]] std::wstring target_version() { return std::wstring(target_version_label); }

[[nodiscard]] std::string profile_fingerprint() {
  return std::string(source_version_label_utf8) + "-to-" +
         std::string(target_version_label_utf8) + "-" +
         std::to_string(patch_plan.size());
}

[[nodiscard]] std::filesystem::path work_root(const std::filesystem::path& game_root) {
  return game_root / L".skyrim-runtime-swapper";
}

[[nodiscard]] std::filesystem::path active_root(const std::filesystem::path& game_root) {
  return work_root(game_root) / L"transaction";
}

[[nodiscard]] std::filesystem::path journal_path(const std::filesystem::path& game_root) {
  return active_root(game_root) / L"runtime.journal";
}

[[nodiscard]] std::filesystem::path session_marker(const std::filesystem::path& game_root) {
  return work_root(game_root) / L"target-session.pending";
}

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

[[nodiscard]] DowngradeResult probe_backend(const std::filesystem::path& game_root) {
  const auto probe = transaction_backend().probe(game_root);
  if (!probe.success()) return {probe.code, false, probe.message};
  return {ExitCode::success, false, probe.description};
}

[[nodiscard]] bool clean_transaction_tree(const std::filesystem::path& game_root) {
  auto& backend = transaction_backend();
  const auto active = active_root(game_root);
  const auto journal = journal_path(game_root);
  const auto recovery_journal = active / L"recovery.journal";
  bool success = true;
  if (std::filesystem::is_regular_file(journal)) success = backend.durable_remove(journal);
  if (std::filesystem::is_regular_file(recovery_journal)) {
    success = backend.durable_remove(recovery_journal) && success;
  }
  std::error_code error;
  std::filesystem::remove_all(active, error);
  return success && !error;
}

[[nodiscard]] DowngradeResult recover_to_source(const std::filesystem::path& game_root,
                                                const std::filesystem::path& patch_root) {
  auto& backend = transaction_backend();
  const auto backend_result = probe_backend(game_root);
  if (!backend_result.success()) return backend_result;

  const auto active = active_root(game_root);
  const auto journal = journal_path(game_root);
  const auto marker = session_marker(game_root);
  const auto journal_state = read_transaction_journal(journal);
  std::error_code error;
  const auto marker_status = inspect_regular_file(marker, error);
  if (marker_status != RegularFileStatus::missing &&
      marker_status != RegularFileStatus::regular) {
    return {ExitCode::recovery_failed, false,
            L"The runtime session marker could not be inspected."};
  }
  const bool marker_exists = marker_status == RegularFileStatus::regular;

  std::vector<FileState> states;
  states.reserve(patch_plan.size());
  std::size_t source_count{};
  std::size_t target_count{};
  std::size_t unknown_count{};
  bool preflight_failed = false;
  std::wstring preflight_message;

  for (std::size_t index = 0; index < patch_plan.size(); ++index) {
    const auto& plan = patch_plan[index];
    const auto live = game_root / utf8_path(plan.relative_file);
    const auto rollback = active / L"rollback" / std::to_wstring(index);
    const auto state = inspect_file(live, plan);
    states.push_back(state);
    if (state == FileState::source) {
      ++source_count;
      continue;
    }
    if (state == FileState::target) {
      ++target_count;
      continue;
    }
    ++unknown_count;
    if (!plan.source_present ||
        (!hash_matches(rollback, plan.source_sha256) &&
         !has_verified_source_backup(game_root, plan))) {
      preflight_failed = true;
      preflight_message = L"A game file is neither supported nor recoverable: " +
                          quote_path(live);
      break;
    }
  }

  const bool mixed_runtime = source_count != 0 && target_count != 0;
  const DWORD active_attributes = GetFileAttributesW(active.c_str());
  const DWORD active_error = active_attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
  if (active_attributes == INVALID_FILE_ATTRIBUTES && active_error != ERROR_FILE_NOT_FOUND &&
      active_error != ERROR_PATH_NOT_FOUND) {
    return {ExitCode::recovery_failed, false,
            L"The runtime transaction directory could not be inspected."};
  }
  const bool active_exists = active_attributes != INVALID_FILE_ATTRIBUTES;
  const bool transaction_present = journal_state.status != JournalReadStatus::missing ||
                                   marker_exists || active_exists;
  if (!transaction_present && !mixed_runtime && source_count == patch_plan.size()) {
    return {ExitCode::success, false, L"Skyrim " + source_version() + L" is already active."};
  }
  if (!transaction_present && !mixed_runtime && target_count == patch_plan.size()) {
    return {ExitCode::success, false, L"Skyrim " + target_version() + L" is already active."};
  }
  if (!transaction_present && unknown_count == 0 && source_count == 0 &&
      target_count == 0) {
    return {ExitCode::success, false, L"No interrupted runtime transaction was found."};
  }
  for (std::size_t index = 0; index < patch_plan.size() && !preflight_failed; ++index) {
    if (states[index] != FileState::target) continue;
    const auto& plan = patch_plan[index];
    const auto rollback = active / L"rollback" / std::to_wstring(index);
    const auto patch = patch_root / utf8_path(plan.reverse_patch);
    if (plan.source_present && !hash_matches(rollback, plan.source_sha256) &&
        !hash_matches(patch, plan.reverse_patch_sha256) &&
        !has_verified_source_backup(game_root, plan)) {
      preflight_failed = true;
      preflight_message =
          L"Neither the reverse patch nor the verified fallback backup is available for: " +
          quote_path(game_root / utf8_path(plan.relative_file));
    }
  }
  if (preflight_failed) {
    return {ExitCode::recovery_failed, false,
            preflight_message + L"\n\nUse Steam's Verify integrity of game files action."};
  }

  std::filesystem::create_directories(active / L"recovery", error);
  if (error) {
    return {ExitCode::recovery_failed, false,
            L"The runtime recovery directory could not be created."};
  }
  const auto recovery_journal_path = active / L"recovery.journal";
  if (std::filesystem::is_regular_file(recovery_journal_path) &&
      !backend.durable_remove(recovery_journal_path)) {
    return {ExitCode::recovery_failed, false,
            L"A stale runtime recovery journal could not be replaced."};
  }
  TransactionJournal recovery(recovery_journal_path, make_transaction_id(),
                              profile_fingerprint(), false);
  if (!recovery.append(JournalPhase::recovery_started,
                       std::numeric_limits<std::uint32_t>::max())) {
    return {ExitCode::recovery_failed, false,
            L"The runtime recovery journal could not be written."};
  }

  bool changed = false;
  bool backup_fallback_used = false;
  for (std::size_t index = 0; index < patch_plan.size(); ++index) {
    const auto& plan = patch_plan[index];
    const auto live = game_root / utf8_path(plan.relative_file);
    const auto rollback = active / L"rollback" / std::to_wstring(index);
    if (states[index] == FileState::source) continue;

    if (!plan.source_present) {
      if (!recovery.append(JournalPhase::replace_pending,
                           static_cast<std::uint32_t>(index), plan.source_sha256) ||
          !backend.durable_remove(live) ||
          !matches_state(live, plan.source_present, plan.source_sha256)) {
        return {ExitCode::recovery_failed, changed,
                L"An added target-runtime file could not be removed: " + quote_path(live)};
      }
    } else {
      if (!recovery.append(JournalPhase::replace_pending,
                           static_cast<std::uint32_t>(index),
                           plan.source_sha256)) {
        return {ExitCode::recovery_failed, changed,
                L"A pending recovery step could not be recorded."};
      }

      bool restored = false;
      if (hash_matches(rollback, plan.source_sha256)) {
        restored = backend.restore_file(rollback, live) &&
                   hash_matches(live, plan.source_sha256);
      } else if (states[index] == FileState::target) {
        const auto staged = active / L"recovery" / std::to_wstring(index);
        const auto reverse_patch = patch_root / utf8_path(plan.reverse_patch);
        if (hash_matches(reverse_patch, plan.reverse_patch_sha256)) {
          const auto patched = apply_hdiff_patch(live, reverse_patch, staged);
          if (patched.success && backend.flush_file(staged) &&
              hash_matches(staged, plan.source_sha256)) {
            const auto discarded = active / L"discarded" / std::to_wstring(index);
            restored = backend.atomic_replace(live, staged, discarded) &&
                       hash_matches(live, plan.source_sha256);
          }
        }
      }

      if (!restored && restore_source_backup(game_root, plan, live)) {
        restored = true;
        backup_fallback_used = true;
      }
      if (!restored) {
        return {ExitCode::recovery_failed, changed,
                L"A source-runtime file could not be restored from its reverse patch or "
                L"fallback backup: " + quote_path(live)};
      }

      const auto discarded = active / L"discarded" / std::to_wstring(index);
      if (std::filesystem::is_regular_file(discarded) && !backend.durable_remove(discarded)) {
        return {ExitCode::recovery_failed, true,
                L"A temporary recovery file could not be removed safely."};
      }
    }
    changed = true;
    if (!recovery.append(JournalPhase::recovery_file, static_cast<std::uint32_t>(index),
                         plan.source_sha256)) {
      return {ExitCode::recovery_failed, changed,
              L"The completed recovery step could not be recorded."};
    }
  }

  for (std::size_t index = 0; index < patch_plan.size(); ++index) {
    const auto& plan = patch_plan[index];
    const auto live = game_root / utf8_path(plan.relative_file);
    if (!matches_state(live, plan.source_present, plan.source_sha256) &&
        !(plan.source_present && restore_source_backup(game_root, plan, live) &&
          (backup_fallback_used = true))) {
      return {ExitCode::recovery_failed, changed,
              L"Final source-runtime verification failed: " + quote_path(live)};
    }
  }
  if (!recovery.append(JournalPhase::recovery_completed,
                       std::numeric_limits<std::uint32_t>::max())) {
    return {ExitCode::recovery_failed, changed,
            L"The completed recovery could not be recorded."};
  }
  if (std::filesystem::is_regular_file(marker) && !backend.durable_remove(marker)) {
    return {ExitCode::recovery_failed, changed,
            L"The stale runtime session marker could not be removed."};
  }
  if (!clean_transaction_tree(game_root)) {
    return {ExitCode::recovery_failed, changed,
            L"Runtime recovery completed, but transaction cleanup failed."};
  }
  return {ExitCode::success, changed,
          changed ? L"Skyrim " + source_version() + L" was recovered successfully." +
                        (backup_fallback_used
                             ? L" A verified fallback backup was used."
                             : L"")
                  : L"The interrupted transaction was cleaned up."};
}

}  // namespace

DowngradeResult recover_runtime_transaction(const std::filesystem::path& game_root,
                                            const std::filesystem::path& patch_root) {
  try {
    return recover_to_source(game_root, patch_root);
  } catch (const std::exception&) {
    return {ExitCode::internal_error, false,
            L"An unexpected internal error occurred during runtime recovery."};
  }
}

DowngradeResult transform_runtime(const std::filesystem::path& game_root,
                                  const std::filesystem::path& patch_root,
                                  bool to_target, bool recover_first) {
  try {
    if (!to_target) return recover_to_source(game_root, patch_root);

    if (recover_first) {
      const auto recovered = recover_to_source(game_root, patch_root);
      if (!recovered.success()) return recovered;
    }

    auto& backend = transaction_backend();
    const auto active = active_root(game_root);
    std::error_code error;
    std::filesystem::create_directories(active / L"staged", error);
    std::filesystem::create_directories(active / L"rollback", error);
    if (error) {
      return {ExitCode::patch_failed, false,
              L"The runtime transaction directory could not be created."};
    }

    std::vector<WorkItem> work;
    std::uint64_t required_space = 64ULL * 1024ULL * 1024ULL;
    for (std::size_t index = 0; index < patch_plan.size(); ++index) {
      const auto& plan = patch_plan[index];
      const auto live = game_root / utf8_path(plan.relative_file);
      const auto state = inspect_file(live, plan);
      if (state == FileState::unknown) {
        (void)clean_transaction_tree(game_root);
        return {ExitCode::source_hash_mismatch, false,
                L"Unknown, modified, or unexpected game file: " + quote_path(live)};
      }
      if (state == FileState::target) continue;

      WorkItem item{
          .index = index,
          .plan = &plan,
          .live = live,
          .patch = patch_root / utf8_path(plan.forward_patch),
          .staged = active / L"staged" / std::to_wstring(index),
          .rollback = active / L"rollback" / std::to_wstring(index),
      };
      if (!hash_matches(item.patch, plan.forward_patch_sha256)) {
        (void)clean_transaction_tree(game_root);
        return {ExitCode::patch_files_missing, false,
                L"A required patch is missing or modified for " + quote_path(live)};
      }
      required_space += plan.target_size;
      work.push_back(std::move(item));
    }

    if (work.empty()) {
      (void)clean_transaction_tree(game_root);
      return {ExitCode::success, false,
              L"Skyrim " + target_version() + L" is already active."};
    }
    required_space += required_source_backup_space(game_root);
    if (!has_minimum_free_space(game_root, required_space)) {
      (void)clean_transaction_tree(game_root);
      return {ExitCode::insufficient_disk_space, false,
              L"There is not enough free space to stage the verified runtime swap."};
    }

    const auto backup_result = ensure_source_backups(game_root);
    if (!backup_result.success()) {
      (void)clean_transaction_tree(game_root);
      return {backup_result.code, false, backup_result.message};
    }

    TransactionJournal journal(journal_path(game_root), make_transaction_id(),
                               profile_fingerprint(), true);
    if (!journal.append(JournalPhase::begin,
                        std::numeric_limits<std::uint32_t>::max())) {
      (void)clean_transaction_tree(game_root);
      return {ExitCode::commit_failed, false,
              L"The runtime transaction journal could not be created."};
    }

    for (const auto& item : work) {
      auto patch_input = item.live;
      if (!item.plan->source_present) {
        patch_input = active / L"empty-input" / std::to_wstring(item.index);
        if (!backend.write_atomic(patch_input, {})) {
          const auto rollback = recover_to_source(game_root, patch_root);
          return {ExitCode::patch_failed, !rollback.success(),
                  L"An empty patch input could not be prepared: " + quote_path(item.live)};
        }
      }
      const auto patched = apply_hdiff_patch(patch_input, item.patch, item.staged);
      if (!patched.success || !backend.flush_file(item.staged) ||
          !hash_matches(item.staged, item.plan->target_sha256) ||
          !journal.append(JournalPhase::staged, static_cast<std::uint32_t>(item.index),
                          item.plan->target_sha256)) {
        const auto rollback = recover_to_source(game_root, patch_root);
        return {ExitCode::patch_failed, !rollback.success(),
                L"A staged runtime file failed verification: " + quote_path(item.live)};
      }
    }

    for (const auto& item : work) {
      if (!journal.append(JournalPhase::replace_pending,
                          static_cast<std::uint32_t>(item.index),
                          item.plan->target_sha256) ||
          !(item.plan->source_present
                ? backend.atomic_replace(item.live, item.staged, item.rollback)
                : backend.atomic_install(item.staged, item.live)) ||
          !hash_matches(item.live, item.plan->target_sha256) ||
          !journal.append(JournalPhase::replaced, static_cast<std::uint32_t>(item.index),
                          item.plan->target_sha256)) {
        const auto rollback = recover_to_source(game_root, patch_root);
        return {ExitCode::commit_failed, !rollback.success(),
                rollback.success()
                    ? L"The runtime swap failed and Skyrim was restored."
                    : L"The runtime swap failed and automatic recovery was incomplete."};
      }
    }

    const auto marker_text =
        "Skyrim " + std::string(target_version_label_utf8) + " session pending\n";
    if (!backend.write_atomic(session_marker(game_root), marker_text) ||
        !journal.append(JournalPhase::session_committed,
                        std::numeric_limits<std::uint32_t>::max())) {
      const auto rollback = recover_to_source(game_root, patch_root);
      return {ExitCode::commit_failed, !rollback.success(),
              L"The runtime session could not be committed durably."};
    }

    for (const auto& item : work) {
      if (std::filesystem::is_regular_file(item.rollback) &&
          !backend.durable_remove(item.rollback)) {
        const auto rollback = recover_to_source(game_root, patch_root);
        return {ExitCode::commit_failed, !rollback.success(),
                L"A temporary original could not be cleaned up safely."};
      }
      if (!journal.append(JournalPhase::cleanup, static_cast<std::uint32_t>(item.index))) {
        const auto rollback = recover_to_source(game_root, patch_root);
        return {ExitCode::commit_failed, !rollback.success(),
                L"Runtime transaction cleanup could not be recorded."};
      }
    }
    if (!journal.append(JournalPhase::completed,
                        std::numeric_limits<std::uint32_t>::max())) {
      const auto rollback = recover_to_source(game_root, patch_root);
      return {ExitCode::commit_failed, !rollback.success(),
              L"The completed runtime transaction could not be recorded."};
    }

    return {ExitCode::success, true,
            L"Skyrim was switched from " + source_version() + L" to " + target_version() +
                L"." +
                (backup_result.changed
                     ? L" A verified fallback backup of the managed 1.7.104 files was created."
                     : L"")};
  } catch (const std::exception&) {
    return {ExitCode::internal_error, false,
            L"An unexpected internal error occurred during the runtime swap."};
  }
}

}  // namespace runtime_swapper::core
