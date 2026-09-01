#include "storage_operations.hpp"

#include "content_catalog.hpp"
#include "creation_club.hpp"
#include "fixed_runtime.hpp"

#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/prepared_storage.hpp>
#include <runtime_swapper/recovery_vault.hpp>
#include <runtime_swapper/runtime_version.hpp>

#include <utility>

namespace runtime_swapper::app {
namespace {

constexpr std::string_view restore_intent_name = "persistent-restore";
constexpr std::string_view restore_intent = "SRS-PERSISTENT-RESTORE-1\n";

[[nodiscard]] int mode_rank(SafetyMode mode) noexcept {
  switch (mode) {
    case SafetyMode::automatic:
      return 0;
    case SafetyMode::persistent_only:
      return 1;
    case SafetyMode::persistent_with_warning:
      return 2;
    case SafetyMode::hard_blocked:
      return 3;
  }
  return 3;
}

[[nodiscard]] StorageOperation operations_for(SafetyMode mode) noexcept {
  const auto persistent = StorageOperation::activate_persistent |
                          StorageOperation::restore_persistent |
                          StorageOperation::recover;
  return mode == SafetyMode::automatic
             ? persistent | StorageOperation::activate_session
             : (mode == SafetyMode::hard_blocked ? StorageOperation::none
                                                  : persistent);
}

[[nodiscard]] InstallationOperationResult failure(
    ExitCode code, BackendProbeResult backend, std::wstring message,
    bool changed = false,
    RecoveryLifecyclePhase phase = RecoveryLifecyclePhase::inspect,
    std::wstring technical_detail = {}) {
  InstallationOperationResult result;
  result.code = code;
  result.backend = std::move(backend);
  result.changed = changed;
  result.lifecycle_phase = phase;
  result.technical_detail = technical_detail.empty() ? message
                                                      : std::move(technical_detail);
  result.message = std::move(message);
  return result;
}

[[nodiscard]] InstallationOperationResult recovered_source(
    const std::filesystem::path& game_root, BackendProbeResult backend) {
  const auto lifecycle = inspect_recovery_lifecycle(game_root);
  if (!lifecycle) {
    return failure(ExitCode::journal_corrupt, std::move(backend),
                   L"The recovery lifecycle metadata is invalid.");
  }
  if (*lifecycle != RecoveryLifecycleState::clean_source &&
      *lifecycle != RecoveryLifecycleState::restoring &&
      !transition_recovery_lifecycle(game_root,
                                     RecoveryLifecycleState::restoring)) {
    return failure(ExitCode::commit_failed, std::move(backend),
                   L"The pending recovery state could not be committed.");
  }
  const auto runtime = recover_runtime(game_root);
  if (!runtime.success()) {
    return failure(runtime.code, std::move(backend), runtime.message,
                   runtime.changed_files);
  }
  const auto creation_club = recover_creation_club_content(game_root);
  if (!creation_club.success) {
    return failure(ExitCode::creation_club_cleanup_failed, std::move(backend),
                   creation_club.message,
                   runtime.changed_files || creation_club.changed);
  }
  const auto catalog = recover_content_catalog(game_root);
  if (!catalog.success) {
    return failure(ExitCode::content_catalog_cleanup_failed, std::move(backend),
                   catalog.message,
                   runtime.changed_files || creation_club.changed || catalog.changed);
  }
  const auto fixed = disable_fixed_runtime(game_root);
  if (!fixed.success) {
    return failure(ExitCode::commit_failed, std::move(backend), fixed.message,
                   runtime.changed_files || creation_club.changed ||
                       catalog.changed);
  }
  const auto cleanup = finalize_recovery_storage(game_root, backend);
  if (!cleanup.success()) {
    return failure(cleanup.code, std::move(backend),
                   L"The source state is verified, but recovery cleanup remains pending: " +
                       cleanup.technical_detail,
                   runtime.changed_files || creation_club.changed || catalog.changed,
                   cleanup.phase, cleanup.technical_detail);
  }
  InstallationOperationResult result;
  result.code = ExitCode::success;
  result.backend = std::move(backend);
  result.runtime_changed = runtime.changed_files;
  result.creation_club_changed = creation_club.changed;
  result.content_catalog_changed = catalog.changed;
  result.lifecycle_state = RecoveryLifecycleState::clean_source;
  result.lifecycle_phase = RecoveryLifecyclePhase::complete;
  result.changed = result.runtime_changed || result.creation_club_changed ||
                   result.content_catalog_changed;
  result.message = L"The installation is in a verified source state.";
  return result;
}

[[nodiscard]] InstallationOperationResult finish_restore(
    const std::filesystem::path& game_root, BackendProbeResult backend,
    bool intent_already_written) {
  if (!intent_already_written &&
      !write_recovery_metadata(game_root, restore_intent_name, restore_intent)) {
    return failure(ExitCode::commit_failed, std::move(backend),
                   L"The persistent restore intent could not be committed to the vault.");
  }
  if (!transition_recovery_lifecycle(game_root,
                                     RecoveryLifecycleState::restoring)) {
    return failure(ExitCode::commit_failed, std::move(backend),
                   L"The persistent restore state could not be committed.");
  }

  const auto creation_club = recover_creation_club_content(game_root);
  const auto catalog = recover_content_catalog(game_root);
  const auto runtime = restore_runtime(game_root);
  if (!creation_club.success || !catalog.success || !runtime.success()) {
    std::wstring message = L"Persistent restore remains pending in the recovery vault.";
    if (!runtime.success()) message += L"\n" + runtime.message;
    if (!creation_club.success) message += L"\n" + creation_club.message;
    if (!catalog.success) message += L"\n" + catalog.message;
    return failure(!runtime.success() ? runtime.code : ExitCode::recovery_failed,
                   std::move(backend), std::move(message),
                   runtime.changed_files || creation_club.changed || catalog.changed);
  }

  const auto persistent = clear_persistent_runtime(game_root);
  const auto fixed = disable_fixed_runtime(game_root);
  if (!persistent.success() || !fixed.success ||
      !remove_recovery_metadata(game_root, restore_intent_name)) {
    return failure(ExitCode::commit_failed, std::move(backend),
                   L"Skyrim 1.7.104 was restored, but persistent metadata cleanup is still "
                   L"pending.", true);
  }

  const auto cleanup = finalize_recovery_storage(game_root, backend);
  if (!cleanup.success()) {
    return failure(cleanup.code, std::move(backend),
                   L"Skyrim 1.7.104 was verified, but recovery cleanup remains pending: " +
                       cleanup.technical_detail,
                   true, cleanup.phase, cleanup.technical_detail);
  }

  InstallationOperationResult result;
  result.code = ExitCode::success;
  result.backend = std::move(backend);
  result.changed = runtime.changed_files || creation_club.changed || catalog.changed;
  result.runtime_changed = runtime.changed_files;
  result.creation_club_changed = creation_club.changed;
  result.content_catalog_changed = catalog.changed;
  result.lifecycle_state = RecoveryLifecycleState::clean_source;
  result.lifecycle_phase = RecoveryLifecyclePhase::complete;
  result.message = L"Skyrim 1.7.104 and all persistently managed content were restored.";
  return result;
}

[[nodiscard]] InstallationOperationResult repair_persistent(
    const std::filesystem::path& game_root, BackendProbeResult backend,
    bool risk_accepted, bool catalog_persistent) {
  auto source = recovered_source(game_root, backend);
  if (!source.success()) return source;
  if (!transition_recovery_lifecycle(game_root,
                                     RecoveryLifecycleState::preparing)) {
    return failure(ExitCode::commit_failed, std::move(backend),
                   L"The persistent preparation state could not be committed.");
  }

  auto runtime = downgrade_runtime_persistent_after_recovery(
      game_root, game_root / L"RuntimeSwap" / L"patches", risk_accepted);
  if (!runtime.success()) {
    return failure(runtime.code, std::move(backend), runtime.message,
                   runtime.changed_files);
  }
  auto creation_club = quarantine_creation_club_content(game_root, true);
  if (!creation_club.success) {
    (void)restore_runtime(game_root);
    return failure(ExitCode::creation_club_cleanup_failed, std::move(backend),
                   creation_club.message, true);
  }
  auto catalog = remove_incompatible_content_catalog(game_root, catalog_persistent);
  if (!catalog.success) {
    (void)recover_creation_club_content(game_root);
    (void)restore_runtime(game_root);
    return failure(ExitCode::content_catalog_cleanup_failed, std::move(backend),
                   catalog.message, true);
  }
  if (!target_runtime_is_active(game_root) ||
      !verify_persistent_creation_club_content(game_root).success ||
      (catalog_persistent &&
       !verify_persistent_content_catalog(game_root).success)) {
    return failure(ExitCode::commit_failed, std::move(backend),
                   L"The persistent target state failed final verification.", true);
  }
  if (!transition_recovery_lifecycle(game_root,
                                     RecoveryLifecycleState::target_active)) {
    return failure(ExitCode::commit_failed, std::move(backend),
                   L"The verified target state could not be committed.", true);
  }

  const auto marker = commit_persistent_runtime(game_root, risk_accepted,
                                                catalog_persistent);
  const auto fixed = marker.success() ? enable_fixed_runtime(game_root)
                                      : FixedRuntimeResult{};
  const auto finalized = fixed.success ? finalize_fixed_target_runtime(game_root)
                                       : DowngradeResult{};
  if (!marker.success() || !fixed.success || !finalized.success()) {
    return failure(ExitCode::commit_failed, std::move(backend),
                   !marker.success() ? marker.message
                                     : (!fixed.success ? fixed.message
                                                       : finalized.message),
                   true);
  }
  if (!transition_recovery_lifecycle(game_root,
                                     RecoveryLifecycleState::persistent)) {
    return failure(ExitCode::commit_failed, std::move(backend),
                   L"The persistent lifecycle state could not be committed.", true);
  }

  InstallationOperationResult result;
  result.code = ExitCode::success;
  result.backend = std::move(backend);
  result.changed = true;
  result.persistent = true;
  result.runtime_changed = runtime.changed_files;
  result.creation_club_changed = creation_club.changed;
  result.content_catalog_changed = catalog.changed;
  result.content_catalog_persistent = catalog_persistent;
  result.lifecycle_state = RecoveryLifecycleState::persistent;
  result.lifecycle_phase = RecoveryLifecyclePhase::complete;
  result.message = L"Skyrim " + std::wstring(target_version_label) +
                   L" is active persistently with verified recovery in " +
                   result.backend.vault_path.wstring() + L".";
  return result;
}

}  // namespace

InstallationOperationResult probe_installation_storage(
    const std::filesystem::path& game_root) {
  InstallationOperationResult result;
  result.backend = probe_prepared_storage(game_root);
  if (!result.backend.success() &&
      result.backend.technical_reason.starts_with(L"active-vault")) {
    const auto catalog = inspect_content_catalog_recovery_state();
    const auto migration = retire_orphaned_recovery_locator(
        game_root, catalog.success);
    if (migration.success() && migration.changed) {
      result.backend = probe_prepared_storage(game_root);
      result.changed = true;
      result.lifecycle_state = RecoveryLifecycleState::clean_source;
      result.lifecycle_phase = migration.phase;
      result.technical_detail = migration.technical_detail;
    }
  }
  result.code = result.backend.code;
  result.message = result.backend.message;
  if (!result.backend.success()) return result;

  const auto catalog = probe_content_catalog_storage();
  if (!catalog.success()) {
    result.code = catalog.code;
    result.backend.code = catalog.code;
    result.backend.mode = SafetyMode::hard_blocked;
    result.backend.allowed_operations = StorageOperation::none;
    result.backend.technical_reason =
        L"content-catalog:" +
        (catalog.technical_reason.empty() ? L"unavailable"
                                          : catalog.technical_reason);
    result.backend.message =
        L"The ContentCatalog location is not safely recoverable: " +
        catalog.message;
    result.backend.description = L"Hard blocked";
    result.message = result.backend.message;
    return result;
  }

  if (mode_rank(catalog.mode) > mode_rank(result.backend.mode)) {
    result.backend.mode = catalog.mode;
    result.backend.allowed_operations = operations_for(catalog.mode);
    result.backend.description = safety_mode_label(catalog.mode) +
                                 L": ContentCatalog storage requires this mode";
    result.backend.technical_reason =
        L"content-catalog:" + catalog.technical_reason;
    result.backend.message =
        catalog.mode == SafetyMode::persistent_with_warning
            ? L"The ContentCatalog filesystem could not be fully classified. "
              L"The complete installation requires a persistent downgrade with "
              L"active confirmation."
            : L"Automatic ContentCatalog restoration is not considered safe. "
              L"The complete installation requires a persistent downgrade.";
    result.message = result.backend.message;
  }
  return result;
}

InstallationOperationResult prepare_launch(
    const std::filesystem::path& game_root, bool allow_persistent,
    bool risk_accepted) {
  auto initial = probe_installation_storage(game_root);
  if (!initial.success()) return initial;

  const auto persistent =
      inspect_persistent_runtime(game_root, nullptr, nullptr, false);
  if (persistent == PersistentRuntimeState::invalid) {
    return failure(ExitCode::journal_corrupt, std::move(initial.backend),
                   L"The persistent recovery markers are inconsistent. Skyrim was not "
                   L"started.");
  }
  const bool needs_consent =
      initial.backend.mode != SafetyMode::automatic &&
      persistent == PersistentRuntimeState::inactive;
  if (needs_consent && !allow_persistent) {
    initial.code = ExitCode::user_cancelled;
    initial.message = L"A persistent downgrade requires active confirmation.";
    return initial;
  }
  if (initial.backend.mode == SafetyMode::persistent_with_warning &&
      persistent == PersistentRuntimeState::inactive && !risk_accepted) {
    return failure(ExitCode::invalid_arguments, std::move(initial.backend),
                   L"This unclassified filesystem requires active confirmation.");
  }

  std::wstring context_error;
  auto context = prepare_storage_context(game_root, 0, &context_error);
  if (!context) {
    return failure(ExitCode::unsupported_filesystem, std::move(initial.backend),
                   L"The prepared storage context could not be established: " +
                       context_error);
  }
  if (context->backend.installation_id != initial.backend.installation_id ||
      context->backend.target_volume.stable_id !=
          initial.backend.target_volume.stable_id ||
      context->backend.vault_volume.stable_id !=
          initial.backend.vault_volume.stable_id ||
      context->backend.vault_path != initial.backend.vault_path) {
    return failure(
        ExitCode::unsupported_filesystem, std::move(initial.backend),
        L"The installation or recovery-vault identity changed while the launch was "
        L"being prepared. No managed file was changed.");
  }
  PreparedStorageScope prepared_scope(*context);
  return initial.backend.mode == SafetyMode::automatic &&
                 persistent == PersistentRuntimeState::inactive
             ? activate_session_target(game_root)
             : activate_persistent_target(game_root, risk_accepted);
}

InstallationOperationResult recover_installation(
    const std::filesystem::path& game_root) {
  auto probed = probe_installation_storage(game_root);
  if (!probed.success()) return probed;
  auto backend = std::move(probed.backend);

  const auto restore_intent_state =
      read_recovery_metadata(game_root, restore_intent_name);
  if (restore_intent_state) {
    if (*restore_intent_state != restore_intent) {
      return failure(ExitCode::journal_corrupt, std::move(backend),
                     L"The persistent restore intent is invalid.");
    }
    return finish_restore(game_root, std::move(backend), true);
  }

  bool risk_accepted = false;
  bool catalog_persistent = false;
  const auto persistent = inspect_persistent_runtime(
      game_root, &risk_accepted, &catalog_persistent);
  if (persistent == PersistentRuntimeState::invalid) {
    return failure(ExitCode::journal_corrupt, std::move(backend),
                   L"The persistent vault and target-volume markers do not agree. No launch is "
                   L"allowed until recovery completes.");
  }
  if (persistent == PersistentRuntimeState::inactive) {
    return recovered_source(game_root, std::move(backend));
  }

  const auto runtime = finalize_fixed_target_runtime(game_root);
  const auto creation_club = verify_persistent_creation_club_content(game_root);
  const auto catalog = catalog_persistent
                           ? verify_persistent_content_catalog(game_root)
                           : recover_content_catalog(game_root);
  if (runtime.success() && creation_club.success && catalog.success) {
    const auto lifecycle = inspect_recovery_lifecycle(game_root);
    if (!lifecycle ||
        (*lifecycle != RecoveryLifecycleState::persistent &&
         !transition_recovery_lifecycle(
             game_root, RecoveryLifecycleState::persistent))) {
      return failure(ExitCode::commit_failed, std::move(backend),
                     L"The migrated persistent lifecycle could not be committed.");
    }
    InstallationOperationResult result;
    result.code = ExitCode::success;
    result.backend = std::move(backend);
    result.persistent = true;
    result.content_catalog_persistent = catalog_persistent;
    result.content_catalog_changed = catalog.changed;
    result.lifecycle_state = RecoveryLifecycleState::persistent;
    result.lifecycle_phase = RecoveryLifecyclePhase::complete;
    result.changed = catalog.changed;
    result.message = L"The persistent target state and recovery vault are verified.";
    return result;
  }
  return repair_persistent(game_root, std::move(backend), risk_accepted,
                           catalog_persistent);
}

InstallationOperationResult activate_session_target(
    const std::filesystem::path& game_root) {
  auto recovered = recover_installation(game_root);
  if (!recovered.success() || recovered.persistent) return recovered;
  if (!recovered.backend.allows(StorageOperation::activate_session)) {
    return failure(ExitCode::unsupported_filesystem, recovered.backend,
                   L"This volume supports only a persistent downgrade.");
  }
  if (!transition_recovery_lifecycle(game_root,
                                     RecoveryLifecycleState::preparing)) {
    return failure(ExitCode::commit_failed, recovered.backend,
                   L"The session preparation state could not be committed.");
  }

  auto runtime = downgrade_runtime_after_recovery(
      game_root, game_root / L"RuntimeSwap" / L"patches");
  if (!runtime.success()) {
    return failure(runtime.code, recovered.backend, runtime.message,
                   runtime.changed_files);
  }
  const auto creation_club = quarantine_creation_club_content(game_root);
  const auto catalog = creation_club.success
                           ? remove_incompatible_content_catalog(game_root)
                           : ContentCatalogResult{};
  if (!creation_club.success || !catalog.success) {
    if (creation_club.changed) (void)recover_creation_club_content(game_root);
    if (catalog.changed) (void)recover_content_catalog(game_root);
    if (runtime.changed_files) (void)restore_runtime(game_root);
    return failure(!creation_club.success ? ExitCode::creation_club_cleanup_failed
                                         : ExitCode::content_catalog_cleanup_failed,
                   recovered.backend,
                   !creation_club.success ? creation_club.message : catalog.message,
                   true);
  }
  if (!transition_recovery_lifecycle(game_root,
                                     RecoveryLifecycleState::target_active)) {
    (void)recover_creation_club_content(game_root);
    (void)recover_content_catalog(game_root);
    (void)restore_runtime(game_root);
    return failure(ExitCode::commit_failed, recovered.backend,
                   L"The verified session target state could not be committed.", true);
  }

  recovered.changed = runtime.changed_files || creation_club.changed || catalog.changed;
  recovered.runtime_changed = runtime.changed_files;
  recovered.creation_club_changed = creation_club.changed;
  recovered.content_catalog_changed = catalog.changed;
  recovered.lifecycle_state = RecoveryLifecycleState::target_active;
  recovered.lifecycle_phase = RecoveryLifecyclePhase::complete;
  recovered.message = runtime.message;
  return recovered;
}

InstallationOperationResult activate_persistent_target(
    const std::filesystem::path& game_root, bool risk_accepted) {
  auto recovered = recover_installation(game_root);
  if (!recovered.success()) return recovered;
  if (!recovered.backend.allows(StorageOperation::activate_persistent)) {
    return failure(ExitCode::unsupported_filesystem, recovered.backend,
                   recovered.backend.message);
  }
  if (!recovered.persistent &&
      recovered.backend.mode == SafetyMode::persistent_with_warning &&
      !risk_accepted) {
    return failure(ExitCode::invalid_arguments, recovered.backend,
                   L"This unclassified filesystem requires active confirmation.");
  }

  const auto catalog_backend = probe_content_catalog_storage();
  const bool catalog_persistent =
      catalog_backend.success() && catalog_backend.mode != SafetyMode::automatic;
  if (recovered.persistent) {
    const auto catalog = catalog_persistent
                             ? verify_persistent_content_catalog(game_root)
                             : remove_incompatible_content_catalog(game_root);
    if (!catalog.success) {
      return failure(ExitCode::content_catalog_cleanup_failed, recovered.backend,
                     catalog.message, catalog.changed);
    }
    recovered.content_catalog_changed = catalog.changed;
    recovered.content_catalog_persistent = catalog_persistent;
    recovered.changed = recovered.changed || catalog.changed;
    return recovered;
  }
  return repair_persistent(game_root, recovered.backend, risk_accepted,
                           catalog_persistent);
}

InstallationOperationResult restore_persistent_source(
    const std::filesystem::path& game_root) {
  auto probed = probe_installation_storage(game_root);
  if (!probed.success()) return probed;
  return finish_restore(game_root, std::move(probed.backend), false);
}

}  // namespace runtime_swapper::app
