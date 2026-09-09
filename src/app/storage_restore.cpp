#include "storage_restore.hpp"

#include "content_catalog.hpp"
#include "creation_club.hpp"
#include "fixed_runtime.hpp"
#include "storage_operation_support.hpp"

#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/recovery_vault.hpp>

namespace runtime_swapper::app {
using detail::elapsed_milliseconds;
using detail::failure;
using detail::SteadyClock;

[[nodiscard]] InstallationOperationResult
finish_restore(const std::filesystem::path &game_root,
               BackendProbeResult backend) {
  // The caller holds the installation lock. Never trust a GUI snapshot from
  // before another process completed a restore.
  const auto stored_intent = read_recovery_metadata(game_root, restore_intent_name);
  if (stored_intent.failed()) {
    return failure(ExitCode::recovery_failed, std::move(backend),
                   L"The persistent restore intent could not be read safely (" +
                       std::wstring(recovery_metadata_status_name(stored_intent.status)) + L").");
  }
  if (stored_intent.present() && stored_intent.contents != restore_intent) {
    return failure(ExitCode::journal_corrupt, std::move(backend),
                   L"The persistent restore intent is invalid.");
  }
  const auto lifecycle = inspect_recovery_lifecycle(game_root);
  if (!lifecycle) {
    return failure(ExitCode::journal_corrupt, std::move(backend),
                   L"The recovery lifecycle metadata is invalid.");
  }
  const bool source_phase = *lifecycle == RecoveryLifecycleState::clean_source ||
                            *lifecycle == RecoveryLifecycleState::source_verified;
  if (source_phase && !source_runtime_is_active(game_root)) {
    return failure(ExitCode::recovery_failed, std::move(backend),
                   L"The recorded source state does not match the complete runtime. "
                   L"Recovery data has been retained.", false,
                   RecoveryLifecyclePhase::verify_source);
  }
  // Already-restored installations still need supplemental recovery and
  // verified cleanup, but must not receive a new restore intent or re-enter
  // restoring. This also retires the orphaned intent left by older versions.
  if (!source_phase) {
    if (!stored_intent.present() &&
        !write_recovery_metadata(game_root, restore_intent_name, restore_intent)) {
      return failure(ExitCode::commit_failed, std::move(backend),
                     L"The persistent restore intent could not be committed to the vault.");
    }
    if (!transition_recovery_lifecycle(game_root, RecoveryLifecycleState::restoring)) {
      return failure(ExitCode::commit_failed, std::move(backend),
                     L"The persistent restore state could not be committed.");
    }
  }

  const auto restore_started = SteadyClock::now();
  const auto creation_club = recover_creation_club_content(game_root);
  const auto catalog = recover_content_catalog(game_root);
  const auto runtime = restore_runtime(game_root);
  if (!creation_club.success || !catalog.success || !runtime.success()) {
    std::wstring message =
        L"Persistent restore remains pending in the recovery vault.";
    if (!runtime.success())
      message += L"\n" + runtime.message;
    if (!creation_club.success)
      message += L"\n" + creation_club.message;
    if (!catalog.success)
      message += L"\n" + catalog.message;
    return failure(
        !runtime.success() ? runtime.code : ExitCode::recovery_failed,
        std::move(backend), std::move(message),
        runtime.changed_files || creation_club.changed || catalog.changed);
  }

  if (!source_runtime_is_active(game_root)) {
    return failure(ExitCode::recovery_failed, std::move(backend),
                   L"The complete source runtime could not be verified. "
                   L"Recovery metadata has been retained.",
                   runtime.changed_files || creation_club.changed || catalog.changed,
                   RecoveryLifecyclePhase::verify_source);
  }

  const auto persistent = clear_persistent_runtime(game_root);
  const auto fixed = disable_fixed_runtime(game_root);
  if (!persistent.success() || !fixed.success ||
      !remove_recovery_metadata(game_root, restore_intent_name)) {
    return failure(ExitCode::commit_failed, std::move(backend),
                   L"Skyrim 1.7.104 was restored, but persistent metadata "
                   L"cleanup is still "
                   L"pending.",
                   true);
  }

  const auto restore_duration = elapsed_milliseconds(restore_started);
  const auto cleanup_started = SteadyClock::now();
  const auto cleanup = finalize_recovery_storage(game_root, backend);
  if (!cleanup.success()) {
    return failure(
        cleanup.code, std::move(backend),
        L"Skyrim 1.7.104 was verified, but recovery cleanup remains pending: " +
            cleanup.technical_detail,
        true, cleanup.phase, cleanup.technical_detail);
  }
  const auto cleanup_duration = elapsed_milliseconds(cleanup_started);

  InstallationOperationResult result;
  result.code = ExitCode::success;
  result.backend = std::move(backend);
  result.changed =
      runtime.changed_files || creation_club.changed || catalog.changed;
  result.runtime_changed = runtime.changed_files;
  result.creation_club_changed = creation_club.changed;
  result.content_catalog_changed = catalog.changed;
  result.lifecycle_state = RecoveryLifecycleState::clean_source;
  result.lifecycle_phase = RecoveryLifecyclePhase::complete;
  result.message =
      L"Skyrim 1.7.104 and all persistently managed content were restored."
      L"\nPerformance: restore=" +
      std::to_wstring(restore_duration) + L" ms, cleanup=" +
      std::to_wstring(cleanup_duration) + L" ms";
  if (!cleanup.technical_detail.empty())
    result.message += L"\n" + cleanup.technical_detail;
  result.technical_detail = cleanup.technical_detail;
  return result;
}


}  // namespace runtime_swapper::app
