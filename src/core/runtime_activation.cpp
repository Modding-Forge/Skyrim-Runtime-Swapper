#include "internal/runtime_transform.hpp"

#include "internal/file_operations.hpp"
#include "internal/runtime_backup.hpp"
#include "internal/runtime_transaction_support.hpp"
#include "internal/transaction_journal.hpp"
#include "internal/transaction_workspace.hpp"
#include "internal/vault_store.hpp"

#include <runtime_swapper/checked_arithmetic.hpp>
#include <runtime_swapper/hdiff_patch.hpp>
#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/prepared_storage.hpp>
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
#include <system_error>
#include <utility>
#include <vector>

namespace runtime_swapper::core {
namespace {

struct WorkItem {
  std::size_t index{};
  const PatchPlanEntry* plan{};
  ManagedFilePath managed;
  std::filesystem::path patch;
  std::filesystem::path staged;
  std::filesystem::path rollback;
  std::filesystem::path empty_input;
  std::optional<std::string> input_sha256;
  std::optional<std::string> patch_sha256;
  bool cached_target{};
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

void add_unique_directory(std::vector<std::filesystem::path>& directories,
                          const std::filesystem::path& directory) {
  if (std::ranges::find(directories, directory) == directories.end()) {
    directories.push_back(directory);
  }
}

[[nodiscard]] bool sync_directories(
    TransactionBackend& backend,
    const std::vector<std::filesystem::path>& directories) {
  return std::ranges::all_of(directories, [&backend](const auto& directory) {
    return static_cast<bool>(backend.sync_directory(directory));
  });
}

}  // namespace

DowngradeResult transform_runtime(const std::filesystem::path& game_root,
                                  const std::filesystem::path& patch_root,
                                  bool to_target, bool recover_first,
                                  bool risk_accepted) {
  try {
    if (!to_target) return recover_to_source_internal(game_root, patch_root, true);

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
      const auto recovered = recover_to_source_internal(game_root, patch_root);
      recovery_duration = elapsed_milliseconds(recovery_started);
      if (!recovered.success()) return recovered;
    }
    const auto runtime_layout = detect_runtime_layout(game_root);

    const auto preflight_started = SteadyClock::now();
    auto& backend = transaction_backend();
    const auto storage_probe = probe_prepared_storage(game_root);
    if (!storage_probe.success() || storage_probe.transaction_work.value.empty() ||
        !storage_probe.transaction_work.value.is_absolute()) {
      return {ExitCode::unsupported_filesystem, false,
              L"A safe target-volume transaction workspace is unavailable."};
    }
    const auto transaction_id = make_transaction_id();
    const auto active = transaction_root(storage_probe, transaction_id);
    if (active.empty()) {
      return {ExitCode::internal_error, false,
              L"A unique runtime transaction path could not be created."};
    }
    std::error_code error;

    std::vector<WorkItem> work;
    std::vector<ManagedFilePath> transaction_files;
    transaction_files.reserve(patch_plan.size());
    std::uint64_t required_space = 64ULL * 1024ULL * 1024ULL;
    for (std::size_t index = 0; index < patch_plan.size(); ++index) {
      const auto& plan = patch_plan[index];
      if (!patch_plan_entry_enabled(runtime_layout, plan)) continue;
      std::wstring path_error;
      const auto managed = resolve_managed_file(
          game_root, utf8_path(plan.relative_file), &path_error);
      if (!managed || (!plan.source_present && managed->redirected)) {
        clean_runtime_transaction_best_effort(game_root, active);
        return {ExitCode::source_hash_mismatch, false,
                managed
                    ? L"An added runtime file is unexpectedly redirected: " +
                          quote_path(managed->logical)
                    : L"A managed runtime path is unsafe: " + path_error};
      }
      const auto& live = managed->effective;
      const auto paths = resolve_runtime_transaction_paths(
          backend, active, *managed, transaction_id, index);
      if (!paths) {
        clean_runtime_transaction_best_effort(game_root, active);
        return {ExitCode::unsupported_filesystem, false,
                L"The effective runtime file cannot be replaced atomically on "
                L"its current mount: " + quote_path(managed->logical) +
                    managed_link_verification_detail(*managed)};
      }
      transaction_files.push_back(*managed);
      const auto inspection = inspect_runtime_file(live, plan);
      if (inspection.state == FileState::unknown) {
        clean_runtime_transaction_best_effort(game_root, active);
        return {ExitCode::source_hash_mismatch, false,
                L"Unknown, modified, or unexpected game file: " +
                    quote_path(managed->logical) +
                    runtime_hash_verification_detail(
                        plan, inspection.actual_sha256) +
                    managed_link_verification_detail(*managed)};
      }
      if (inspection.state == FileState::target) continue;

      WorkItem item{
          .index = index,
          .plan = &plan,
          .managed = *managed,
          .patch = patch_root / utf8_path(plan.forward_patch),
          .staged = paths->staged,
          .rollback = paths->rollback,
          .empty_input = paths->empty_input,
          .input_sha256 = inspection.actual_sha256,
      };
      if (!checked_add(required_space, plan.target_size, required_space)) {
        clean_runtime_transaction_best_effort(game_root, active);
        return {ExitCode::internal_error, false,
                L"The runtime staging size exceeds the supported range."};
      }
      work.push_back(std::move(item));
    }

    if (work.empty()) {
      clean_runtime_transaction_best_effort(game_root, active);
      return {ExitCode::success, false,
              L"Skyrim " + target_version() + L" is already active."};
    }
    if (!has_minimum_free_space(game_root, required_space)) {
      clean_runtime_transaction_best_effort(game_root, active);
      return {ExitCode::insufficient_disk_space, false,
              L"There is not enough free space to stage the verified runtime swap."};
    }
    preflight_duration = elapsed_milliseconds(preflight_started);

    const auto source_vault_started = SteadyClock::now();
    const auto backup_result = ensure_source_backups(game_root);
    if (!backup_result.success()) {
      clean_runtime_transaction_best_effort(game_root, active);
      return {backup_result.code, false, backup_result.message};
    }

    const auto vault = resolve_vault_layout(game_root);
    if (!vault || vault->runtime_layout != runtime_layout ||
        !runtime_layout_matches(game_root, runtime_layout) ||
        !runtime_manifest_matches(*vault)) {
      clean_runtime_transaction_best_effort(game_root, active);
      return {ExitCode::backup_failed, false,
              L"The verified recovery-vault manifest became unavailable."};
    }
    source_vault_duration = elapsed_milliseconds(source_vault_started);

    const auto staging_started = SteadyClock::now();
    const bool cache_targets = vault->probe.mode == SafetyMode::automatic;
    const auto target_cache = cache_targets
                                  ? resolve_target_cache_layout(game_root)
                                  : std::nullopt;
    std::vector<JournalAppend> staged_records;
    staged_records.reserve(work.size());
    for (auto& item : work) {
      if (target_cache && item.plan->target_present) {
        ++target_cache_candidates;
        item.cached_target = target_cache_object_available(
            *target_cache, item.plan->target_sha256, item.plan->target_size);
      }
      const auto patch_verification =
          verify_hash(item.patch, item.plan->forward_patch_sha256);
      item.patch_sha256 = patch_verification.actual;
      if (!patch_verification.matches) {
        clean_runtime_transaction_best_effort(game_root, active);
        return {ExitCode::patch_files_missing, false,
                L"A required patch is missing or modified for " +
                    quote_path(item.managed.logical) +
                    hash_verification_detail(
                        L"Expected patch SHA-256", true,
                        item.plan->forward_patch_sha256,
                        patch_verification.actual) +
                    managed_link_verification_detail(item.managed)};
      }
    }
    const auto transaction_profile =
        profile_fingerprint(game_root, transaction_files, runtime_layout);
    if (transaction_profile.empty()) {
      clean_runtime_transaction_best_effort(game_root, active);
      return {ExitCode::commit_failed, false,
              L"The managed runtime layout could not be authenticated."};
    }
    TransactionJournal journal(runtime_journal_path(*vault), transaction_id,
                               transaction_profile, true, risk_accepted);
    if (!journal.append(JournalPhase::begin,
                        std::numeric_limits<std::uint32_t>::max())) {
      clean_runtime_transaction_best_effort(game_root, active);
      return {ExitCode::commit_failed, false,
              L"The runtime transaction journal could not be created."};
    }
    std::filesystem::create_directories(active / L"staged", error);
    std::filesystem::create_directories(active / L"rollback", error);
    if (error) {
      return {ExitCode::patch_failed, false,
              L"The runtime transaction directory could not be created."};
    }

    for (auto& item : work) {
      if (!runtime_layout_matches(game_root, runtime_layout) ||
          !managed_file_mapping_matches(game_root, item.managed)) {
        const auto rollback = recover_to_source_internal(game_root, patch_root);
        return {ExitCode::patch_failed, !rollback.success(),
                L"A managed runtime path changed while staging: " +
                    quote_path(item.managed.logical) +
                    managed_link_verification_detail(item.managed)};
      }
      bool staged = false;
      std::optional<std::string> staged_sha256;
      std::wstring staging_detail;
      if (item.cached_target) {
        staged = materialize_target_cache_object(
            *target_cache, item.plan->target_sha256, item.plan->target_size,
            item.staged);
        if (staged) {
          ++target_cache_hits;
          staged_sha256 = std::string(item.plan->target_sha256);
        }
      }
      if (!staged) {
        item.cached_target = false;
        auto patch_input = item.managed.effective;
        if (!item.plan->source_present) {
          patch_input = item.empty_input;
          if (!backend.write_atomic(patch_input, {})) {
            const auto rollback = recover_to_source_internal(game_root, patch_root);
            return {ExitCode::patch_failed, !rollback.success(),
                    L"An empty patch input could not be prepared: " +
                        quote_path(item.managed.logical)};
          }
        }
        const auto patched = apply_hdiff_patch(patch_input, item.patch, item.staged);
        if (!item.plan->source_present) {
          const auto removed_empty = backend.durable_remove(item.empty_input);
          if (!removed_empty) {
            const auto rollback = recover_to_source_internal(game_root, patch_root);
            return {ExitCode::patch_failed, !rollback.success(),
                    L"The temporary empty patch input could not be removed: " +
                        quote_path(item.managed.logical)};
          }
        }
        if (!patched.success) {
          staging_detail = patched.error;
        } else {
          const auto flushed = backend.flush_file(item.staged);
          if (!flushed) {
            staging_detail = mutation_failure_detail(flushed);
          } else {
            const auto verification =
                verify_hash(item.staged, item.plan->target_sha256);
            staged = verification.matches;
            staged_sha256 = verification.actual;
          }
        }
        if (staged && target_cache && item.plan->target_present) {
          (void)commit_target_cache_object(
              *target_cache, item.staged, item.plan->target_sha256,
              item.plan->target_size);
        }
      }
      if (!staged) {
        const auto rollback = recover_to_source_internal(game_root, patch_root);
        return {ExitCode::patch_failed, !rollback.success(),
                L"A staged runtime file failed verification: " +
                    quote_path(item.managed.logical) +
                    L"\nStaged object: " + quote_path(item.staged) +
                    runtime_hash_verification_detail(*item.plan,
                                                     staged_sha256) +
                    L"\nPatch input actual SHA-256: " +
                    (item.input_sha256
                         ? std::wstring(item.input_sha256->begin(),
                                        item.input_sha256->end())
                         : L"<unavailable>") +
                    L"\nExpected forward-patch SHA-256: " +
                    std::wstring(item.plan->forward_patch_sha256.begin(),
                                 item.plan->forward_patch_sha256.end()) +
                    L"\nActual forward-patch SHA-256: " +
                    (item.patch_sha256
                         ? std::wstring(item.patch_sha256->begin(),
                                        item.patch_sha256->end())
                         : L"<unavailable>") +
                    managed_link_verification_detail(item.managed) +
                    (staging_detail.empty()
                         ? L""
                         : L"\nPatch detail: " + staging_detail)};
      }
      staged_records.push_back(
          {JournalPhase::staged, static_cast<std::uint32_t>(item.index),
           item.plan->target_sha256});
    }
    if (!journal.append_batch(staged_records)) {
      const auto rollback = recover_to_source_internal(game_root, patch_root);
      return {ExitCode::patch_failed, !rollback.success(),
              L"The staged runtime boundary could not be committed."};
    }
    staging_duration = elapsed_milliseconds(staging_started);

    const auto commit_started = SteadyClock::now();
    std::vector<JournalAppend> replace_intents;
    replace_intents.reserve(work.size());
    for (const auto& item : work) {
      if (!runtime_layout_matches(game_root, runtime_layout) ||
          !managed_file_mapping_matches(game_root, item.managed)) {
        const auto rollback = recover_to_source_internal(game_root, patch_root);
        return {ExitCode::commit_failed, !rollback.success(),
                L"A managed runtime path changed before commit."};
      }
      replace_intents.push_back(
          {JournalPhase::replace_pending,
           static_cast<std::uint32_t>(item.index),
           item.plan->target_sha256});
    }
    if (!journal.append_batch(replace_intents)) {
      const auto rollback = recover_to_source_internal(game_root, patch_root);
      return {ExitCode::commit_failed, !rollback.success(),
              L"The complete runtime replacement intent could not be committed."};
    }

    std::vector<JournalAppend> replaced_records;
    replaced_records.reserve(work.size());
    std::vector<std::filesystem::path> commit_directories;
    for (const auto& item : work) {
      const bool layout_matches =
          runtime_layout_matches(game_root, runtime_layout) &&
          managed_file_mapping_matches(game_root, item.managed);
      MutationResult installed;
      if (layout_matches) {
        installed = item.plan->source_present
                        ? backend.atomic_replace_deferred_sync(
                              item.managed.effective, item.staged,
                              item.rollback)
                        : backend.atomic_install_deferred_sync(
                              item.staged, item.managed.effective);
      }
      HashVerification live_verification;
      if (layout_matches && installed) {
        live_verification =
            verify_hash(item.managed.effective, item.plan->target_sha256);
      }
      if (!layout_matches || !installed || !live_verification.matches) {
        const auto rollback = recover_to_source_internal(game_root, patch_root);
        return {ExitCode::commit_failed, !rollback.success(),
                (rollback.success()
                     ? L"The runtime swap failed and Skyrim was restored: "
                     : L"The runtime swap failed and automatic recovery was "
                       L"incomplete: ") +
                    quote_path(item.managed.logical) +
                    runtime_hash_verification_detail(
                        *item.plan, live_verification.actual) +
                    managed_link_verification_detail(item.managed) +
                    (!layout_matches || installed
                         ? L""
                         : L"\n" + mutation_failure_detail(installed))};
      }
      replaced_records.push_back(
          {JournalPhase::replaced, static_cast<std::uint32_t>(item.index),
           item.plan->target_sha256});
      add_unique_directory(commit_directories,
                           item.managed.effective.parent_path());
      add_unique_directory(commit_directories, item.staged.parent_path());
      if (item.plan->source_present) {
        add_unique_directory(commit_directories, item.rollback.parent_path());
      }
    }
    if (!sync_directories(backend, commit_directories)) {
      const auto rollback = recover_to_source_internal(game_root, patch_root);
      return {ExitCode::commit_failed, !rollback.success(),
              L"The runtime directory boundary could not be synchronized."};
    }
    if (!journal.append_batch(replaced_records)) {
      const auto rollback = recover_to_source_internal(game_root, patch_root);
      return {ExitCode::commit_failed, !rollback.success(),
              L"The completed runtime replacement boundary could not be committed."};
    }

    const auto marker_text =
        "Skyrim " + std::string(target_version_label_utf8) + " session pending\n";
    if (!backend.write_atomic(workspace_session_marker(vault->probe), marker_text) ||
        !journal.append(JournalPhase::session_committed,
                        std::numeric_limits<std::uint32_t>::max())) {
      const auto rollback = recover_to_source_internal(game_root, patch_root);
      return {ExitCode::commit_failed, !rollback.success(),
              L"The runtime session could not be committed durably."};
    }

    std::vector<JournalAppend> cleanup_records;
    cleanup_records.reserve(work.size());
    std::vector<std::filesystem::path> cleanup_directories;
    for (const auto& item : work) {
      if (std::filesystem::is_regular_file(item.rollback) &&
          !backend.durable_remove_deferred_sync(item.rollback)) {
        const auto rollback = recover_to_source_internal(game_root, patch_root);
        return {ExitCode::commit_failed, !rollback.success(),
                L"A temporary original could not be cleaned up safely."};
      }
      cleanup_records.push_back(
          {JournalPhase::cleanup, static_cast<std::uint32_t>(item.index), {}});
      if (item.plan->source_present) {
        add_unique_directory(cleanup_directories, item.rollback.parent_path());
      }
    }
    if (!sync_directories(backend, cleanup_directories)) {
      const auto rollback = recover_to_source_internal(game_root, patch_root);
      return {ExitCode::commit_failed, !rollback.success(),
              L"The runtime cleanup boundary could not be synchronized."};
    }
    if (!journal.append_batch(cleanup_records)) {
      const auto rollback = recover_to_source_internal(game_root, patch_root);
      return {ExitCode::commit_failed, !rollback.success(),
              L"Runtime transaction cleanup could not be recorded."};
    }
    if (!journal.append(JournalPhase::completed,
                        std::numeric_limits<std::uint32_t>::max())) {
      const auto rollback = recover_to_source_internal(game_root, patch_root);
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
