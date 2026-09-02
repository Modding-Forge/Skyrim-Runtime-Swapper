#include "internal/runtime_transform.hpp"

#include "internal/file_operations.hpp"
#include "internal/runtime_backup.hpp"
#include "internal/runtime_transaction_support.hpp"
#include "internal/transaction_journal.hpp"
#include "internal/transaction_workspace.hpp"
#include "internal/vault_store.hpp"

#include <runtime_swapper/hdiff_patch.hpp>
#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/prepared_storage.hpp>
#include <runtime_swapper/checked_arithmetic.hpp>
#include <runtime_swapper/release_version.hpp>
#include <runtime_swapper/runtime_layout.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace runtime_swapper::core {
namespace {

struct CleanupResult {
  bool success{};
  std::wstring detail;
};

[[nodiscard]] std::string legacy_profile_name(RuntimeLayout layout) {
  std::string fingerprint = std::string(source_version_label_utf8) + "-to-" +
         std::string(target_version_label_utf8) + "-" +
         std::to_string(patch_plan.size());
  if (layout != RuntimeLayout::standard) {
    fingerprint += "-" + std::string(runtime_layout_name(layout));
  }
  return fingerprint;
}

[[nodiscard]] std::string legacy_profile_fingerprint(
    const std::filesystem::path& game_root,
    const std::vector<ManagedFilePath>& managed_files, RuntimeLayout runtime_layout) {
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
  return hash ? legacy_profile_name(runtime_layout) + "-" + hash->substr(0, 8)
               : std::string{};
}

[[nodiscard]] std::string journal_truncated_profile(std::string profile) {
  constexpr std::size_t disk_profile_size = 32;
  if (profile.size() > disk_profile_size) profile.resize(disk_profile_size);
  return profile;
}

[[nodiscard]] bool recognized_legacy_profile(
    std::string_view recorded, const std::filesystem::path& game_root,
    const std::vector<ManagedFilePath>& managed_files,
    RuntimeLayout runtime_layout) {
  return recorded == journal_truncated_profile(legacy_profile_name(runtime_layout)) ||
         recorded == journal_truncated_profile(legacy_profile_fingerprint(
                         game_root, managed_files, runtime_layout));
}

[[nodiscard]] bool compact_profile_format(std::string_view profile) {
  if (profile.size() != 31 || !profile.starts_with("p3-")) return false;
  return std::ranges::all_of(profile.substr(3), [](char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
  });
}

[[nodiscard]] std::filesystem::path legacy_journal_path(
    const std::filesystem::path& game_root) {
  return legacy_transaction_root(game_root) / L"runtime.journal";
}

[[nodiscard]] std::filesystem::path legacy_session_marker(
    const std::filesystem::path& game_root) {
  return legacy_installation_work_root(game_root) / L"target-session.pending";
}

[[nodiscard]] std::filesystem::path journal_transaction_root(
    const BackendProbeResult& probe, const JournalReadResult& journal) {
  if (journal.status != JournalReadStatus::valid || journal.records.empty()) {
    return {};
  }
  return transaction_root(probe, journal.records.front().transaction_id);
}

[[nodiscard]] bool path_exists(const std::filesystem::path& path) {
  if (path.empty()) return false;
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  return !error && std::filesystem::exists(status);
}

[[nodiscard]] std::filesystem::path existing_transaction_root(
    const std::filesystem::path& game_root, const BackendProbeResult& probe,
    const JournalReadResult& journal, std::string_view fresh_transaction_id) {
  const auto external = journal_transaction_root(probe, journal);
  if (path_exists(external)) return external;
  if (!external.empty() && path_exists(legacy_transaction_root(game_root))) {
    // RC5 through RC11 journaled a random transaction ID while their staged
    // files lived in the deterministic Skyrim-local tree.
    return legacy_transaction_root(game_root);
  }
  if (!external.empty()) return external;
  return transaction_root(probe, fresh_transaction_id);
}

[[nodiscard]] std::filesystem::path existing_session_marker(
    const std::filesystem::path& game_root, const BackendProbeResult& probe) {
  const auto external = workspace_session_marker(probe);
  return path_exists(external) ? external : legacy_session_marker(game_root);
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

[[nodiscard]] CleanupResult remove_empty_transaction_parent(
    const std::filesystem::path& root) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(root, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && !std::filesystem::exists(status))) {
    return {true, {}};
  }
  if (error || !std::filesystem::is_directory(status) ||
      managed_path_entry_is_redirected(root)) {
    return cleanup_failure(L"Refused to inspect an unsafe legacy transaction parent",
                           root, error);
  }
  if (!std::filesystem::is_empty(root, error)) {
    return error ? cleanup_failure(L"Could not inspect the legacy transaction parent",
                                   root, error)
                 : CleanupResult{true, {}};
  }
  if (!std::filesystem::remove(root, error) || error ||
      !transaction_backend().sync_parent(root)) {
    return cleanup_failure(L"Could not remove the empty legacy transaction parent",
                           root, error);
  }
  return {true, {}};
}

[[nodiscard]] CleanupResult clean_transaction_tree(
    const std::filesystem::path& game_root,
    const std::filesystem::path& active) {
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
  const auto active_cleanup = remove_transaction_directory(active);
  if (!active_cleanup.success) return active_cleanup;
  const auto legacy_active = legacy_transaction_root(game_root);
  if (active != legacy_active) {
    const auto legacy_cleanup = remove_transaction_directory(legacy_active);
    if (!legacy_cleanup.success) return legacy_cleanup;
  }
  return remove_empty_transaction_parent(
      legacy_installation_work_root(game_root));
}

struct SourceCandidateRestore {
  bool success{};
  std::wstring detail;
};

[[nodiscard]] SourceCandidateRestore restore_source_candidate(
    const std::filesystem::path& game_root, const PatchPlanEntry& plan,
    const std::filesystem::path& live, const std::filesystem::path& staged,
    const std::filesystem::path& discarded) {
  auto& backend = transaction_backend();
  if (!hash_matches(staged, plan.source_sha256)) {
    std::error_code staged_error;
    const auto staged_status = inspect_regular_file(staged, staged_error);
    if (staged_status == RegularFileStatus::regular) {
      const auto removed = backend.durable_remove(staged);
      if (!removed) return {false, mutation_failure_detail(removed)};
    } else if (staged_status != RegularFileStatus::missing) {
      return {false, L"The source recovery candidate is not a regular file."};
    }
    if (!materialize_source_backup(game_root, plan, staged) ||
        !hash_matches(staged, plan.source_sha256)) {
      return {false, L"The verified source backup could not be materialized."};
    }
  }

  std::error_code live_error;
  const auto live_status = inspect_regular_file(live, live_error);
  MutationResult installed;
  if (live_status == RegularFileStatus::missing) {
    installed = backend.atomic_install(staged, live);
  } else if (live_status == RegularFileStatus::regular) {
    installed = backend.atomic_replace(live, staged, discarded);
  } else {
    return {false, L"The live recovery destination is not a regular file."};
  }
  const auto live_verification = verify_hash(live, plan.source_sha256);
  if (live_verification.matches) return {true, {}};
  return {false,
          installed
              ? L"The installed source backup failed final hash verification." +
                    runtime_hash_verification_detail(plan,
                                                     live_verification.actual)
              : mutation_failure_detail(installed)};
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
  const auto runtime_layout = vault->runtime_layout;
  const auto active_count = active_patch_plan_size(runtime_layout);

  const auto journal = runtime_journal_path(*vault);
  const auto legacy_journal = legacy_journal_path(game_root);
  const auto recovery_path = recovery_journal_path(*vault);
  auto journal_state = read_transaction_journal(journal);
  if (journal_state.status == JournalReadStatus::missing) {
    journal_state = read_transaction_journal(legacy_journal);
  }
  if (journal_state.status == JournalReadStatus::missing) {
    journal_state = read_transaction_journal(recovery_path);
  }
  if (journal_state.status == JournalReadStatus::corrupt) {
    return {ExitCode::recovery_failed, false,
             L"The runtime transaction journal is corrupt. No managed file was changed."};
  }
  const auto fresh_transaction_id = make_transaction_id();
  const auto active = existing_transaction_root(
      game_root, vault->probe, journal_state, fresh_transaction_id);
  if (active.empty()) {
    return {ExitCode::recovery_failed, false,
            L"A unique recovery transaction path could not be resolved."};
  }
  const auto active_transaction_id =
      journal_state.status == JournalReadStatus::valid &&
              !journal_state.records.empty()
          ? journal_state.records.front().transaction_id
          : fresh_transaction_id;
  const auto marker = existing_session_marker(game_root, vault->probe);
  std::error_code error;
  const auto marker_status = inspect_regular_file(marker, error);
  if (marker_status != RegularFileStatus::missing &&
      marker_status != RegularFileStatus::regular) {
    return {ExitCode::recovery_failed, false,
            L"The runtime session marker could not be inspected."};
  }
  const bool marker_exists = marker_status == RegularFileStatus::regular;

  std::vector<FileState> states(patch_plan.size(), FileState::unknown);
  std::vector<std::optional<std::string>> actual_hashes(patch_plan.size());
  std::vector<std::optional<ManagedFilePath>> managed_files(patch_plan.size());
  std::vector<std::optional<RuntimeTransactionPaths>> transaction_paths(
      patch_plan.size());
  std::vector<ManagedFilePath> transaction_files;
  transaction_files.reserve(active_count);
  std::size_t source_count{};
  std::size_t target_count{};
  std::size_t unknown_count{};
  bool preflight_failed = false;
  std::wstring preflight_message;

  for (std::size_t index = 0; index < patch_plan.size(); ++index) {
    const auto& plan = patch_plan[index];
    if (!patch_plan_entry_enabled(runtime_layout, plan)) continue;
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
    managed_files[index] = *managed;
    auto paths = resolve_runtime_transaction_paths(
        backend, active, *managed, active_transaction_id, index);
    if (!paths) {
      preflight_failed = true;
      preflight_message =
          L"A recovery workspace cannot be placed on the effective runtime "
          L"mount: " + quote_path(managed->logical) +
          managed_link_verification_detail(*managed);
      break;
    }
    const auto old_rollback = active / L"rollback" / std::to_wstring(index);
    if (paths->adjacent && path_exists(old_rollback)) {
      // RC12 through RC14 staged all rollbacks in the Steam-library workspace.
      // Keep reading an interrupted legacy rollback, while all new recovery
      // outputs use the mount-local names above.
      paths->rollback = old_rollback;
    }
    transaction_paths[index] = *paths;
    transaction_files.push_back(*managed);
    const auto& live = managed->effective;
    const auto& rollback = paths->rollback;
    const auto inspection = inspect_runtime_file(live, plan);
    states[index] = inspection.state;
    actual_hashes[index] = inspection.actual_sha256;
    if (inspection.state == FileState::source) {
      ++source_count;
      continue;
    }
    if (inspection.state == FileState::target) {
      ++target_count;
      continue;
    }
    ++unknown_count;
    if (!plan.source_present ||
        (!hash_matches(rollback, plan.source_sha256) &&
         !has_verified_source_backup(game_root, plan))) {
      preflight_failed = true;
      preflight_message = L"A game file is neither supported nor recoverable: " +
                          quote_path(managed->logical) +
                          runtime_hash_verification_detail(
                              plan, inspection.actual_sha256) +
                          managed_link_verification_detail(*managed);
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
  const bool transaction_present =
      journal_state.status != JournalReadStatus::missing || marker_exists ||
      active_exists || path_exists(legacy_transaction_root(game_root));
  bool recovery_layout_rebound = false;
  if (!preflight_failed && journal_state.status == JournalReadStatus::valid &&
      !journal_state.records.empty()) {
    const auto current_profile =
        profile_fingerprint(game_root, transaction_files, runtime_layout);
    const auto& recorded_profile = journal_state.records.front().profile;
    if (current_profile.empty()) {
      return {ExitCode::recovery_failed, false,
              L"The current managed runtime layout could not be authenticated. "
              L"Recovery was stopped before writing any file."};
    }
    if (recorded_profile != current_profile) {
      const auto legacy_name =
          journal_truncated_profile(legacy_profile_name(runtime_layout));
      const auto legacy_current = journal_truncated_profile(
          legacy_profile_fingerprint(game_root, transaction_files,
                                     runtime_layout));
      const bool legacy_bound = recorded_profile == legacy_current &&
                                legacy_current != legacy_name;
      const bool recoverable_profile = legacy_bound ||
          recognized_legacy_profile(recorded_profile, game_root,
                                    transaction_files, runtime_layout) ||
          compact_profile_format(recorded_profile);
      if (!recoverable_profile) {
        return {ExitCode::recovery_failed, false,
                L"The pending runtime transaction uses an incompatible layout "
                L"profile. Recovery was stopped before writing any file."};
      }
      recovery_layout_rebound = !legacy_bound;
      if (recovery_layout_rebound && unknown_count != 0) {
        const auto unknown = std::ranges::find(states, FileState::unknown);
        const auto index = static_cast<std::size_t>(unknown - states.begin());
        const auto logical = index < managed_files.size() && managed_files[index]
                                 ? managed_files[index]->logical
                                 : game_root;
        return {ExitCode::recovery_failed, false,
                L"A managed runtime link was rebuilt, but its current content is "
                L"unknown: " + quote_path(logical) +
                L". Recovery was stopped before writing any file." +
                    runtime_hash_verification_detail(
                        patch_plan[index], actual_hashes[index]) +
                    (index < managed_files.size() && managed_files[index]
                         ? managed_link_verification_detail(
                               *managed_files[index])
                         : L"")};
      }
    }
  }
  if (!transaction_present && !mixed_runtime && source_count == active_count) {
    return {ExitCode::success, false, L"Skyrim " + source_version() + L" is already active."};
  }
  if (!restore_clean_target && !transaction_present && !mixed_runtime &&
      target_count == active_count) {
    return {ExitCode::success, false, L"Skyrim " + target_version() + L" is already active."};
  }
  if (!transaction_present && unknown_count == 0 && source_count == 0 &&
      target_count == 0) {
    return {ExitCode::success, false, L"No interrupted runtime transaction was found."};
  }
  for (std::size_t index = 0; index < patch_plan.size() && !preflight_failed; ++index) {
    if (!patch_plan_entry_enabled(runtime_layout, patch_plan[index])) continue;
    if (states[index] != FileState::target) continue;
    const auto& plan = patch_plan[index];
    const auto& rollback = transaction_paths[index]->rollback;
    const auto patch = patch_root / utf8_path(plan.reverse_patch);
    if (plan.source_present && !hash_matches(rollback, plan.source_sha256) &&
        !hash_matches(patch, plan.reverse_patch_sha256) &&
        !has_verified_source_backup(game_root, plan)) {
      preflight_failed = true;
      preflight_message =
          L"Neither the reverse patch nor the verified fallback backup is available for: " +
          quote_path(managed_files[index]->logical);
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
  if (std::filesystem::is_regular_file(recovery_path) &&
      !backend.durable_remove(recovery_path)) {
    return {ExitCode::recovery_failed, false,
            L"A stale runtime recovery journal could not be replaced."};
  }
  if (!runtime_layout_matches(game_root, runtime_layout)) {
    return {ExitCode::recovery_failed, false,
            L"The managed runtime layout changed before recovery began."};
  }
  TransactionJournal recovery(
      recovery_path, active_transaction_id,
      profile_fingerprint(game_root, transaction_files, runtime_layout), false);
  if (!recovery.append(JournalPhase::recovery_started,
                       std::numeric_limits<std::uint32_t>::max())) {
    return {ExitCode::recovery_failed, false,
            L"The runtime recovery journal could not be written."};
  }

  bool changed = false;
  bool backup_fallback_used = false;
  for (std::size_t index = 0; index < patch_plan.size(); ++index) {
    const auto& plan = patch_plan[index];
    if (!patch_plan_entry_enabled(runtime_layout, plan)) continue;
    const auto& managed = *managed_files[index];
    const auto& live = managed.effective;
    const auto& paths = *transaction_paths[index];
    const auto& rollback = paths.rollback;
    if (states[index] == FileState::source) continue;

    if (!runtime_layout_matches(game_root, runtime_layout) ||
        !managed_file_mapping_matches(game_root, managed)) {
      return {ExitCode::recovery_failed, changed,
              L"A managed runtime path changed during recovery: " +
                  quote_path(managed.logical)};
    }

    if (states[index] == FileState::unknown) {
      std::error_code live_error;
      const auto live_status = inspect_regular_file(live, live_error);
      if (live_status == RegularFileStatus::regular) {
        if (!preserve_conflict(*vault, live, recovery.transaction_id())) {
          return {ExitCode::recovery_failed, changed,
                  L"An unknown live file could not be preserved in the recovery vault: " +
                      quote_path(live)};
        }
      } else if (live_status != RegularFileStatus::missing) {
        return {ExitCode::recovery_failed, changed,
                L"An unknown managed path could not be inspected safely: " +
                    quote_path(live)};
      }
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
      std::optional<MutationResult> rollback_restore;
      std::wstring reverse_patch_detail;
      const auto& staged = paths.recovery;
      const auto& discarded = paths.discarded;
      auto used_discarded = discarded;
      if (hash_matches(rollback, plan.source_sha256)) {
        rollback_restore = backend.restore_file(rollback, live);
        used_discarded = paths.rollback_discarded;
        restored = hash_matches(live, plan.source_sha256);
      } else if (states[index] == FileState::target) {
        const auto reverse_patch = patch_root / utf8_path(plan.reverse_patch);
        const auto reverse_patch_verification =
            verify_hash(reverse_patch, plan.reverse_patch_sha256);
        if (reverse_patch_verification.matches) {
          const auto patched = apply_hdiff_patch(live, reverse_patch, staged);
          if (!patched.success) {
            reverse_patch_detail = patched.error;
          } else {
            const auto flushed = backend.flush_file(staged);
            if (!flushed) {
              reverse_patch_detail = mutation_failure_detail(flushed);
            } else {
              const auto reconstructed =
                  verify_hash(staged, plan.source_sha256);
              if (!reconstructed.matches) {
                reverse_patch_detail =
                    L"The reconstructed source hash did not match." +
                    runtime_hash_verification_detail(
                        plan, reconstructed.actual);
              } else {
                const auto replaced =
                    backend.atomic_replace(live, staged, discarded);
                const auto installed_source =
                    verify_hash(live, plan.source_sha256);
                restored = installed_source.matches;
                if (!restored) {
                  reverse_patch_detail = mutation_failure_detail(replaced) +
                                         runtime_hash_verification_detail(
                                             plan, installed_source.actual);
                }
              }
            }
          }
        } else {
          reverse_patch_detail =
              L"The reverse patch failed verification." +
              hash_verification_detail(
                  L"Expected reverse-patch SHA-256", true,
                  plan.reverse_patch_sha256,
                  reverse_patch_verification.actual);
        }
      }

      if (!restored) {
        const auto fallback = restore_source_candidate(
            game_root, plan, live, staged, discarded);
        restored = fallback.success;
        if (restored) {
          backup_fallback_used = true;
        } else if (reverse_patch_detail.empty()) {
          reverse_patch_detail = fallback.detail;
        }
      }
      if (!restored) {
        return {ExitCode::recovery_failed, changed,
                L"A source-runtime file could not be restored from its reverse patch or "
                L"fallback backup: " + quote_path(managed.logical) +
                    managed_link_verification_detail(managed) +
                    (rollback_restore
                         ? L"\n\n" + mutation_failure_detail(*rollback_restore)
                         : reverse_patch_detail.empty()
                               ? L""
                               : L"\n\n" + reverse_patch_detail)};
      }

      if (std::filesystem::is_regular_file(used_discarded) &&
          !backend.durable_remove(used_discarded)) {
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
    if (!patch_plan_entry_enabled(runtime_layout, plan)) continue;
    const auto& managed = *managed_files[index];
    if (!runtime_layout_matches(game_root, runtime_layout) ||
        !managed_file_mapping_matches(game_root, managed)) {
      return {ExitCode::recovery_failed, changed,
              L"A managed runtime path changed before final verification: " +
                  quote_path(managed.logical)};
    }
    const auto& live = managed.effective;
    if (!matches_state(live, plan.source_present, plan.source_sha256)) {
      const auto& paths = *transaction_paths[index];
      const auto& staged = paths.recovery;
      const auto& discarded = paths.discarded;
      const auto fallback = plan.source_present
                                ? restore_source_candidate(game_root, plan, live,
                                                           staged, discarded)
                                : SourceCandidateRestore{};
      if (!fallback.success) {
        return {ExitCode::recovery_failed, changed,
                L"Final source-runtime verification failed: " +
                    quote_path(managed.logical) +
                    managed_link_verification_detail(managed) +
                    (fallback.detail.empty() ? L"" : L"\n\n" + fallback.detail)};
      }
      backup_fallback_used = true;
      if (std::filesystem::is_regular_file(discarded) &&
          !backend.durable_remove(discarded)) {
        return {ExitCode::recovery_failed, true,
                L"A final recovery discard could not be removed safely."};
      }
    }
  }
  if (!recovery.append(JournalPhase::recovery_completed,
                       std::numeric_limits<std::uint32_t>::max())) {
    return {ExitCode::recovery_failed, changed,
            L"The completed recovery could not be recorded."};
  }
  const auto adjacent_cleanup =
      cleanup_adjacent_runtime_transaction_files(transaction_paths);
  if (!adjacent_cleanup.success) {
    return {ExitCode::recovery_failed, changed,
            L"Runtime recovery completed, but mount-local transaction cleanup "
            L"failed.\n\n" + adjacent_cleanup.detail};
  }
  const auto marker_cleanup = remove_transaction_file(marker);
  if (!marker_cleanup.success) {
    return {ExitCode::recovery_failed, changed,
            L"The stale runtime session marker could not be removed.\n\n" +
                marker_cleanup.detail};
  }
  const auto cleanup = clean_transaction_tree(game_root, active);
  if (!cleanup.success) {
    return {ExitCode::recovery_failed, changed,
            L"Runtime recovery completed, but transaction cleanup failed.\n\n" +
                cleanup.detail};
  }
  return {ExitCode::success, changed,
          changed ? L"Skyrim " + source_version() + L" was recovered successfully." +
                         (backup_fallback_used
                              ? L" A verified fallback backup was used."
                              : L"") +
                         (recovery_layout_rebound
                              ? L" Rebuilt managed links were rebound safely."
                              : L"")
                  : recovery_layout_rebound
                        ? L"The source runtime was verified through rebuilt managed "
                          L"links and the interrupted transaction was cleaned up."
                        : L"The interrupted transaction was cleaned up."};
}

}  // namespace

void clean_runtime_transaction_best_effort(
    const std::filesystem::path& game_root,
    const std::filesystem::path& transaction_root) noexcept {
  try {
    (void)clean_transaction_tree(game_root, transaction_root);
  } catch (...) {
  }
}

DowngradeResult recover_to_source_internal(
    const std::filesystem::path& game_root,
    const std::filesystem::path& patch_root, bool restore_clean_target) {
  return recover_to_source(game_root, patch_root, restore_clean_target);
}

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
    const auto runtime_layout = detect_runtime_layout(game_root);
    if (runtime_layout == RuntimeLayout::invalid) return false;
    for (const auto& plan : patch_plan) {
      if (!patch_plan_entry_enabled(runtime_layout, plan)) continue;
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

bool source_runtime_is_active_internal(
    const std::filesystem::path& game_root) noexcept {
  try {
    const auto runtime_layout = detect_runtime_layout(game_root);
    if (runtime_layout == RuntimeLayout::invalid) return false;
    for (const auto& plan : patch_plan) {
      if (!patch_plan_entry_enabled(runtime_layout, plan)) continue;
      const auto managed = resolve_managed_file(
          game_root, utf8_path(plan.relative_file));
      if (!managed || (!plan.source_present && managed->redirected) ||
          !matches_state(managed->effective, plan.source_present,
                         plan.source_sha256)) {
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
    const auto vault = resolve_vault_layout(game_root);
    if (!vault) {
      return {ExitCode::commit_failed, false,
              L"The fixed runtime recovery vault could not be resolved."};
    }
    auto journal = read_transaction_journal(runtime_journal_path(*vault));
    if (journal.status == JournalReadStatus::missing) {
      journal = read_transaction_journal(legacy_journal_path(game_root));
    }
    const auto active = existing_transaction_root(
        game_root, vault->probe, journal, make_transaction_id());
    for (const auto& marker : {workspace_session_marker(vault->probe),
                               legacy_session_marker(game_root)}) {
      const auto marker_cleanup = remove_transaction_file(marker);
      if (!marker_cleanup.success) {
        return {ExitCode::commit_failed, false,
                L"The temporary runtime session marker could not be removed.\n\n" +
                    marker_cleanup.detail};
      }
    }
    const auto cleanup = clean_transaction_tree(game_root, active);
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


}  // namespace runtime_swapper::core
