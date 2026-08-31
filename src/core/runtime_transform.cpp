#include "internal/runtime_transform.hpp"

#include "internal/file_operations.hpp"
#include "internal/runtime_backup.hpp"
#include "internal/transaction_journal.hpp"
#include "internal/vault_store.hpp"

#include <runtime_swapper/hdiff_patch.hpp>
#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <algorithm>
#include <chrono>
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
  ManagedFilePath managed;
  std::filesystem::path patch;
  std::filesystem::path staged;
  std::filesystem::path rollback;
  bool cached_target{};
};

struct CleanupResult {
  bool success{};
  std::wstring detail;
};

using SteadyClock = std::chrono::steady_clock;

[[nodiscard]] std::int64_t elapsed_milliseconds(
    SteadyClock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             SteadyClock::now() - start)
      .count();
}

[[nodiscard]] std::wstring performance_summary(
    std::int64_t total, std::int64_t recovery, std::int64_t preflight,
    std::int64_t source_vault, std::int64_t staging, std::int64_t commit,
    std::size_t cache_hits, std::size_t cache_candidates) {
  return L"\nPerformance: total=" + std::to_wstring(total) +
         L" ms; recovery=" + std::to_wstring(recovery) +
         L" ms; preflight=" + std::to_wstring(preflight) +
         L" ms; source-vault=" + std::to_wstring(source_vault) +
         L" ms; staging=" + std::to_wstring(staging) +
         L" ms; commit=" + std::to_wstring(commit) +
         L" ms; target-cache=" + std::to_wstring(cache_hits) + L"/" +
         std::to_wstring(cache_candidates) + L".";
}

[[nodiscard]] std::wstring source_version() { return std::wstring(source_version_label); }
[[nodiscard]] std::wstring target_version() { return std::wstring(target_version_label); }

[[nodiscard]] std::string profile_fingerprint() {
  return std::string(source_version_label_utf8) + "-to-" +
         std::string(target_version_label_utf8) + "-" +
         std::to_string(patch_plan.size());
}

[[nodiscard]] std::string profile_fingerprint(
    const std::filesystem::path& game_root,
    const std::vector<ManagedFilePath>& managed_files) {
  std::error_code error;
  const auto root = std::filesystem::canonical(game_root, error);
  if (error) return {};
  std::string layout;
  for (const auto& managed : managed_files) {
    const auto relative = managed.effective.lexically_relative(root).generic_u8string();
    layout.push_back(managed.redirected ? 's' : 'f');
    layout.append(reinterpret_cast<const char*>(relative.data()), relative.size());
    layout.push_back('\n');
  }
  const auto hash = sha256_string(layout);
  return hash ? profile_fingerprint() + "-" + hash->substr(0, 8) : std::string{};
}

[[nodiscard]] std::filesystem::path work_root(const std::filesystem::path& game_root) {
  return game_root / L".skyrim-runtime-swapper";
}

[[nodiscard]] std::filesystem::path active_root(const std::filesystem::path& game_root) {
  return work_root(game_root) / L"transaction";
}

[[nodiscard]] std::filesystem::path legacy_journal_path(
    const std::filesystem::path& game_root) {
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

[[nodiscard]] CleanupResult cleanup_failure(
    std::wstring_view action, const std::filesystem::path& path,
    const std::error_code& error = {}) {
  std::wstring detail(action);
  detail += L": ";
  detail += quote_path(path);
  if (error) {
    detail += L" (";
    const auto message = error.message();
    detail.append(message.begin(), message.end());
    detail += L")";
  }
  return {false, std::move(detail)};
}

[[nodiscard]] CleanupResult remove_transaction_file(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) return {true, {}};
  if (error) return cleanup_failure(L"Could not inspect transaction file", path, error);
  if (!std::filesystem::exists(status)) return {true, {}};
  if (!std::filesystem::is_regular_file(status) ||
      managed_path_entry_is_redirected(path)) {
    return cleanup_failure(L"Refused to remove an unsafe transaction entry", path);
  }
  if (!transaction_backend().durable_remove(path)) {
    return cleanup_failure(L"Could not durably remove transaction file", path);
  }
  return {true, {}};
}

[[nodiscard]] CleanupResult remove_transaction_directory(
    const std::filesystem::path& root) {
  std::error_code error;
  const auto root_status = std::filesystem::symlink_status(root, error);
  if (error == std::errc::no_such_file_or_directory) return {true, {}};
  if (error) {
    return cleanup_failure(L"Could not inspect transaction directory", root, error);
  }
  if (!std::filesystem::exists(root_status)) return {true, {}};
  if (!std::filesystem::is_directory(root_status) ||
      managed_path_entry_is_redirected(root)) {
    return cleanup_failure(L"Refused to traverse an unsafe transaction directory",
                           root);
  }

  std::vector<std::filesystem::path> files;
  std::vector<std::filesystem::path> directories;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::none, error);
  const std::filesystem::recursive_directory_iterator end;
  if (error) {
    return cleanup_failure(L"Could not enumerate transaction directory", root, error);
  }
  while (iterator != end) {
    const auto entry = iterator->path();
    const auto status = iterator->symlink_status(error);
    if (error) {
      return cleanup_failure(L"Could not inspect transaction entry", entry, error);
    }
    if (managed_path_entry_is_redirected(entry)) {
      iterator.disable_recursion_pending();
      return cleanup_failure(L"Refused to traverse an unsafe transaction entry", entry);
    }
    if (std::filesystem::is_directory(status)) {
      directories.push_back(entry);
    } else if (std::filesystem::is_regular_file(status)) {
      files.push_back(entry);
    } else {
      return cleanup_failure(L"Refused to remove an unsupported transaction entry",
                             entry);
    }
    iterator.increment(error);
    if (error) {
      return cleanup_failure(L"Could not enumerate transaction directory", root, error);
    }
  }

  for (const auto& file : files) {
    const auto removed = remove_transaction_file(file);
    if (!removed.success) return removed;
  }
  auto& backend = transaction_backend();
  directories.insert(directories.begin(), root);
  for (auto directory = directories.rbegin(); directory != directories.rend();
       ++directory) {
    error.clear();
    const auto current_status = std::filesystem::symlink_status(*directory, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && !std::filesystem::exists(current_status))) {
      continue;
    }
    if (error || !std::filesystem::is_directory(current_status) ||
        managed_path_entry_is_redirected(*directory)) {
      return cleanup_failure(L"Refused to remove an unsafe transaction directory",
                             *directory, error);
    }
    const bool removed = std::filesystem::remove(*directory, error);
    if (error) {
      return cleanup_failure(L"Could not remove transaction directory", *directory,
                             error);
    }
    if (!removed) {
      const auto status = std::filesystem::symlink_status(*directory, error);
      if (error != std::errc::no_such_file_or_directory &&
          (error || std::filesystem::exists(status))) {
        return cleanup_failure(L"Transaction directory was not empty", *directory,
                               error);
      }
    }
    if (removed && !backend.sync_parent(*directory)) {
      return cleanup_failure(L"Could not synchronize transaction cleanup", *directory);
    }
  }
  return {true, {}};
}

[[nodiscard]] CleanupResult clean_transaction_tree(
    const std::filesystem::path& game_root) {
  const auto active = active_root(game_root);
  const auto legacy_journal = legacy_journal_path(game_root);
  const auto vault = resolve_vault_layout(game_root);
  if (!vault) {
    return {false, L"The recovery vault could not be resolved during cleanup."};
  }
  for (const auto& journal : {runtime_journal_path(*vault),
                              recovery_journal_path(*vault), legacy_journal}) {
    const auto removed = remove_transaction_file(journal);
    if (!removed.success) return removed;
  }
  return remove_transaction_directory(active);
}

[[nodiscard]] DowngradeResult recover_to_source(const std::filesystem::path& game_root,
                                                const std::filesystem::path& patch_root,
                                                bool restore_clean_target = false) {
  auto& backend = transaction_backend();
  const auto backend_result = probe_backend(game_root);
  if (!backend_result.success()) return backend_result;

  std::wstring vault_error;
  const auto vault = resolve_vault_layout(game_root, 0, &vault_error);
  if (!vault) {
    return {ExitCode::recovery_failed, false,
            L"The recovery vault is unavailable: " + vault_error};
  }

  const auto active = active_root(game_root);
  const auto journal = runtime_journal_path(*vault);
  const auto legacy_journal = legacy_journal_path(game_root);
  const auto marker = session_marker(game_root);
  auto journal_state = read_transaction_journal(journal);
  if (journal_state.status == JournalReadStatus::missing) {
    journal_state = read_transaction_journal(legacy_journal);
  }
  if (journal_state.status == JournalReadStatus::corrupt) {
    return {ExitCode::recovery_failed, false,
            L"The runtime transaction journal is corrupt. No managed file was changed."};
  }
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
  std::vector<ManagedFilePath> managed_files;
  managed_files.reserve(patch_plan.size());
  std::size_t source_count{};
  std::size_t target_count{};
  std::size_t unknown_count{};
  bool preflight_failed = false;
  std::wstring preflight_message;

  for (std::size_t index = 0; index < patch_plan.size(); ++index) {
    const auto& plan = patch_plan[index];
    std::wstring path_error;
    const auto managed = resolve_managed_file(
        game_root, utf8_path(plan.relative_file), &path_error);
    if (!managed || (!plan.source_present && managed->redirected)) {
      preflight_failed = true;
      preflight_message = managed
                              ? L"An added runtime file is unexpectedly redirected: " +
                                    quote_path(managed->logical)
                              : L"A managed runtime path is unsafe: " + path_error;
      break;
    }
    managed_files.push_back(*managed);
    const auto& live = managed->effective;
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
                          quote_path(managed->logical);
      break;
    }
  }

  const bool mixed_runtime = source_count != 0 && target_count != 0;
  const auto active_status = std::filesystem::symlink_status(active, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    return {ExitCode::recovery_failed, false,
            L"The runtime transaction directory could not be inspected."};
  }
  error.clear();
  const bool active_exists = std::filesystem::exists(active_status);
  const bool transaction_present = journal_state.status != JournalReadStatus::missing ||
                                   marker_exists || active_exists;
  if (journal_state.status == JournalReadStatus::valid &&
      !journal_state.records.empty()) {
    const auto current_profile = profile_fingerprint(game_root, managed_files);
    const bool redirected = std::ranges::any_of(
        managed_files, [](const ManagedFilePath& managed) {
          return managed.redirected;
        });
    const auto& recorded_profile = journal_state.records.front().profile;
    if (current_profile.empty() ||
        (recorded_profile != current_profile &&
         (redirected || recorded_profile != profile_fingerprint()))) {
      return {ExitCode::recovery_failed, false,
              L"A managed runtime link changed since the transaction was committed. "
              L"Recovery was stopped before writing any file."};
    }
  }
  if (!transaction_present && !mixed_runtime && source_count == patch_plan.size()) {
    return {ExitCode::success, false, L"Skyrim " + source_version() + L" is already active."};
  }
  if (!restore_clean_target && !transaction_present && !mixed_runtime &&
      target_count == patch_plan.size()) {
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
          quote_path(managed_files[index].logical);
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
  const auto recovery_path = recovery_journal_path(*vault);
  if (std::filesystem::is_regular_file(recovery_path) &&
      !backend.durable_remove(recovery_path)) {
    return {ExitCode::recovery_failed, false,
            L"A stale runtime recovery journal could not be replaced."};
  }
  TransactionJournal recovery(recovery_path, make_transaction_id(),
                              profile_fingerprint(game_root, managed_files), false);
  if (!recovery.append(JournalPhase::recovery_started,
                       std::numeric_limits<std::uint32_t>::max())) {
    return {ExitCode::recovery_failed, false,
            L"The runtime recovery journal could not be written."};
  }

  bool changed = false;
  bool backup_fallback_used = false;
  for (std::size_t index = 0; index < patch_plan.size(); ++index) {
    const auto& plan = patch_plan[index];
    const auto& managed = managed_files[index];
    const auto& live = managed.effective;
    const auto rollback = active / L"rollback" / std::to_wstring(index);
    if (states[index] == FileState::source) continue;

    if (!managed_file_mapping_matches(game_root, managed)) {
      return {ExitCode::recovery_failed, changed,
              L"A managed runtime path changed during recovery: " +
                  quote_path(managed.logical)};
    }

    if (states[index] == FileState::unknown &&
        !preserve_conflict(*vault, live, recovery.transaction_id())) {
      return {ExitCode::recovery_failed, changed,
              L"An unknown live file could not be preserved in the recovery vault: " +
                  quote_path(live)};
    }

    if (!plan.source_present) {
      if (!recovery.append(JournalPhase::replace_pending,
                           static_cast<std::uint32_t>(index), plan.source_sha256) ||
          !backend.durable_remove(live) ||
          !matches_state(live, plan.source_present, plan.source_sha256)) {
        return {ExitCode::recovery_failed, changed,
                L"An added target-runtime file could not be removed: " +
                    quote_path(managed.logical)};
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
                L"fallback backup: " + quote_path(managed.logical)};
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
    const auto& managed = managed_files[index];
    if (!managed_file_mapping_matches(game_root, managed)) {
      return {ExitCode::recovery_failed, changed,
              L"A managed runtime path changed before final verification: " +
                  quote_path(managed.logical)};
    }
    const auto& live = managed.effective;
    if (!matches_state(live, plan.source_present, plan.source_sha256) &&
        !(plan.source_present && restore_source_backup(game_root, plan, live) &&
          (backup_fallback_used = true))) {
      return {ExitCode::recovery_failed, changed,
              L"Final source-runtime verification failed: " +
                  quote_path(managed.logical)};
    }
  }
  const auto backup_result = ensure_source_backups(game_root);
  if (!backup_result.success()) {
    return {ExitCode::recovery_failed, changed,
            L"The source runtime was recovered, but its complete recovery-vault "
            L"manifest could not be committed: " + backup_result.message};
  }
  if (!recovery.append(JournalPhase::recovery_completed,
                       std::numeric_limits<std::uint32_t>::max())) {
    return {ExitCode::recovery_failed, changed,
            L"The completed recovery could not be recorded."};
  }
  const auto marker_cleanup = remove_transaction_file(marker);
  if (!marker_cleanup.success) {
    return {ExitCode::recovery_failed, changed,
            L"The stale runtime session marker could not be removed.\n\n" +
                marker_cleanup.detail};
  }
  const auto cleanup = clean_transaction_tree(game_root);
  if (!cleanup.success) {
    return {ExitCode::recovery_failed, changed,
            L"Runtime recovery completed, but transaction cleanup failed.\n\n" +
                cleanup.detail};
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

bool target_runtime_is_active_internal(
    const std::filesystem::path& game_root) noexcept {
  try {
    for (const auto& plan : patch_plan) {
      const auto managed = resolve_managed_file(
          game_root, utf8_path(plan.relative_file));
      if (!managed || (!plan.source_present && managed->redirected) ||
          !matches_state(managed->effective, plan.target_present,
                         plan.target_sha256)) {
        return false;
      }
    }
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

DowngradeResult finalize_fixed_target_runtime_internal(
    const std::filesystem::path& game_root) {
  try {
    const auto backend_result = probe_backend(game_root);
    if (!backend_result.success()) return backend_result;
    if (!target_runtime_is_active_internal(game_root)) {
      return {ExitCode::source_hash_mismatch, false,
              L"The managed target runtime is incomplete or modified."};
    }
    const auto marker = session_marker(game_root);
    const auto marker_cleanup = remove_transaction_file(marker);
    if (!marker_cleanup.success) {
      return {ExitCode::commit_failed, false,
              L"The temporary runtime session marker could not be removed.\n\n" +
                  marker_cleanup.detail};
    }
    const auto cleanup = clean_transaction_tree(game_root);
    if (!cleanup.success) {
      return {ExitCode::commit_failed, false,
              L"The fixed target runtime could not be finalized.\n\n" +
                  cleanup.detail};
    }
    return {ExitCode::success, false,
            L"Skyrim " + target_version() + L" is fixed as the active runtime."};
  } catch (const std::exception&) {
    return {ExitCode::internal_error, false,
            L"An unexpected error occurred while finalizing the fixed runtime."};
  }
}

DowngradeResult transform_runtime(const std::filesystem::path& game_root,
                                  const std::filesystem::path& patch_root,
                                  bool to_target, bool recover_first,
                                  bool risk_accepted) {
  try {
    if (!to_target) return recover_to_source(game_root, patch_root, true);

    const auto total_started = SteadyClock::now();
    std::int64_t recovery_duration{};
    std::int64_t preflight_duration{};
    std::int64_t source_vault_duration{};
    std::int64_t staging_duration{};
    std::int64_t commit_duration{};
    std::size_t target_cache_hits{};
    std::size_t target_cache_candidates{};

    if (recover_first) {
      const auto recovery_started = SteadyClock::now();
      const auto recovered = recover_to_source(game_root, patch_root);
      recovery_duration = elapsed_milliseconds(recovery_started);
      if (!recovered.success()) return recovered;
    }

    const auto preflight_started = SteadyClock::now();
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
    std::vector<ManagedFilePath> transaction_files;
    transaction_files.reserve(patch_plan.size());
    std::uint64_t required_space = 64ULL * 1024ULL * 1024ULL;
    for (std::size_t index = 0; index < patch_plan.size(); ++index) {
      const auto& plan = patch_plan[index];
      std::wstring path_error;
      const auto managed = resolve_managed_file(
          game_root, utf8_path(plan.relative_file), &path_error);
      if (!managed || (!plan.source_present && managed->redirected)) {
        (void)clean_transaction_tree(game_root);
        return {ExitCode::source_hash_mismatch, false,
                managed
                    ? L"An added runtime file is unexpectedly redirected: " +
                          quote_path(managed->logical)
                    : L"A managed runtime path is unsafe: " + path_error};
      }
      const auto& live = managed->effective;
      transaction_files.push_back(*managed);
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
          .managed = *managed,
          .patch = patch_root / utf8_path(plan.forward_patch),
          .staged = active / L"staged" / std::to_wstring(index),
          .rollback = active / L"rollback" / std::to_wstring(index),
      };
      required_space += plan.target_size;
      work.push_back(std::move(item));
    }

    if (work.empty()) {
      (void)clean_transaction_tree(game_root);
      return {ExitCode::success, false,
              L"Skyrim " + target_version() + L" is already active."};
    }
    if (!has_minimum_free_space(game_root, required_space)) {
      (void)clean_transaction_tree(game_root);
      return {ExitCode::insufficient_disk_space, false,
              L"There is not enough free space to stage the verified runtime swap."};
    }
    preflight_duration = elapsed_milliseconds(preflight_started);

    const auto source_vault_started = SteadyClock::now();
    const auto backup_result = ensure_source_backups(game_root);
    if (!backup_result.success()) {
      (void)clean_transaction_tree(game_root);
      return {backup_result.code, false, backup_result.message};
    }

    const auto vault = resolve_vault_layout(game_root);
    if (!vault || !runtime_manifest_matches(*vault)) {
      (void)clean_transaction_tree(game_root);
      return {ExitCode::backup_failed, false,
              L"The verified recovery-vault manifest became unavailable."};
    }
    source_vault_duration = elapsed_milliseconds(source_vault_started);

    const auto staging_started = SteadyClock::now();
    const bool cache_targets = vault->probe.mode == SafetyMode::automatic;
    for (auto& item : work) {
      if (cache_targets && item.plan->target_present) {
        ++target_cache_candidates;
        item.cached_target = vault_object_available(
            *vault, item.plan->target_sha256, item.plan->target_size);
      }
      if (!hash_matches(item.patch, item.plan->forward_patch_sha256)) {
        (void)clean_transaction_tree(game_root);
        return {ExitCode::patch_files_missing, false,
                L"A required patch is missing or modified for " +
                    quote_path(item.managed.logical)};
      }
    }
    const auto transaction_profile =
        profile_fingerprint(game_root, transaction_files);
    if (transaction_profile.empty()) {
      (void)clean_transaction_tree(game_root);
      return {ExitCode::commit_failed, false,
              L"The managed runtime layout could not be authenticated."};
    }
    TransactionJournal journal(runtime_journal_path(*vault), make_transaction_id(),
                               transaction_profile, true, risk_accepted);
    if (!journal.append(JournalPhase::begin,
                        std::numeric_limits<std::uint32_t>::max())) {
      (void)clean_transaction_tree(game_root);
      return {ExitCode::commit_failed, false,
              L"The runtime transaction journal could not be created."};
    }

    for (auto& item : work) {
      if (!managed_file_mapping_matches(game_root, item.managed)) {
        const auto rollback = recover_to_source(game_root, patch_root);
        return {ExitCode::patch_failed, !rollback.success(),
                L"A managed runtime path changed while staging: " +
                    quote_path(item.managed.logical)};
      }
      bool staged = false;
      if (item.cached_target) {
        staged = materialize_verified_vault_object(
            *vault, item.plan->target_sha256, item.plan->target_size,
            item.staged);
        if (staged) ++target_cache_hits;
      }
      if (!staged) {
        item.cached_target = false;
        auto patch_input = item.managed.effective;
        if (!item.plan->source_present) {
          patch_input = active / L"empty-input" / std::to_wstring(item.index);
          if (!backend.write_atomic(patch_input, {})) {
            const auto rollback = recover_to_source(game_root, patch_root);
            return {ExitCode::patch_failed, !rollback.success(),
                    L"An empty patch input could not be prepared: " +
                        quote_path(item.managed.logical)};
          }
        }
        const auto patched = apply_hdiff_patch(patch_input, item.patch, item.staged);
        staged = patched.success && backend.flush_file(item.staged) &&
                 hash_matches(item.staged, item.plan->target_sha256);
        if (staged && cache_targets && item.plan->target_present) {
          (void)commit_verified_vault_object(
              *vault, item.staged, item.plan->target_sha256,
              item.plan->target_size);
        }
      }
      if (!staged ||
          !journal.append(JournalPhase::staged,
                          static_cast<std::uint32_t>(item.index),
                          item.plan->target_sha256)) {
        const auto rollback = recover_to_source(game_root, patch_root);
        return {ExitCode::patch_failed, !rollback.success(),
                L"A staged runtime file failed verification: " +
                    quote_path(item.managed.logical)};
      }
    }
    staging_duration = elapsed_milliseconds(staging_started);

    const auto commit_started = SteadyClock::now();
    for (const auto& item : work) {
      if (!managed_file_mapping_matches(game_root, item.managed) ||
          !journal.append(JournalPhase::replace_pending,
                           static_cast<std::uint32_t>(item.index),
                           item.plan->target_sha256) ||
          !(item.plan->source_present
                ? backend.atomic_replace(item.managed.effective, item.staged,
                                         item.rollback)
                : backend.atomic_install(item.staged, item.managed.effective)) ||
          !hash_matches(item.managed.effective, item.plan->target_sha256) ||
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

    commit_duration = elapsed_milliseconds(commit_started);
    const auto total_duration = elapsed_milliseconds(total_started);

    return {ExitCode::success, true,
            L"Skyrim was switched from " + source_version() + L" to " + target_version() +
                L"." +
                (backup_result.changed
                     ? L" A verified fallback backup of the managed 1.7.104 files was created."
                     : L"") +
                performance_summary(total_duration, recovery_duration,
                                    preflight_duration, source_vault_duration,
                                    staging_duration, commit_duration,
                                    target_cache_hits,
                                    target_cache_candidates)};
  } catch (const std::exception&) {
    return {ExitCode::internal_error, false,
            L"An unexpected internal error occurred during the runtime swap."};
  }
}

}  // namespace runtime_swapper::core
