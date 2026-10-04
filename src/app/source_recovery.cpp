#include "storage_restore.hpp"
#include "content_catalog.hpp"
#include "creation_club.hpp"
#include "storage_operation_support.hpp"
#include "internal/source_recovery.hpp"
#include "internal/runtime_transaction_support.hpp"

namespace runtime_swapper::app {
using detail::failure;

std::optional<InstallationOperationResult> reconcile_source_storage(
    const std::filesystem::path& game_root, const BackendProbeResult& backend) {
  const auto inspection = core::inspect_source_recovery(game_root);
  if (inspection.status != core::SourceRecoveryStatus::source_verified) return std::nullopt;
  const auto creation_club = recover_creation_club_content(game_root);
  if (!creation_club.success)
    return failure(ExitCode::creation_club_cleanup_failed, backend, creation_club.message, creation_club.changed);
  const auto catalog = recover_content_catalog(game_root);
  if (!catalog.success)
    return failure(ExitCode::content_catalog_cleanup_failed, backend, catalog.message,
                   creation_club.changed || catalog.changed);
  const auto retired = core::retire_source_recovery(game_root, inspection);
  if (!retired)
    return failure(ExitCode::recovery_failed, backend,
        L"Skyrim 1.7.104 was verified, but prior recovery state could not be retired.\n" +
        core::mutation_failure_detail(retired), true);
  InstallationOperationResult result;
  result.code = ExitCode::success;
  result.backend = backend;
  result.changed = true;
  result.lifecycle_state = RecoveryLifecycleState::clean_source;
  result.lifecycle_phase = RecoveryLifecyclePhase::complete;
  result.message = L"Source-state reconciliation: all files managed by the current and prior "
      L"packages match Skyrim 1.7.104. Supplemental recovery completed; obsolete runtime "
      L"metadata was retired. Non-source recovery data was preserved at: " +
      (backend.vault_path.parent_path() / "source-state-archive").wstring();
  return result;
}

}  // namespace runtime_swapper::app
