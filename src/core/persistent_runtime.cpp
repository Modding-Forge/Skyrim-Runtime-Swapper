#include <runtime_swapper/downgrade.hpp>

#include "internal/vault_store.hpp"

namespace runtime_swapper {

PersistentRuntimeState inspect_persistent_runtime(
    const std::filesystem::path& game_root, bool* risk_accepted,
    bool* catalog_persistent, bool repair_missing_game_marker) {
  const auto vault = core::resolve_vault_layout(
      game_root, 0, nullptr, repair_missing_game_marker);
  if (!vault) return PersistentRuntimeState::invalid;
  switch (core::reconcile_persistent_marker(*vault, game_root, risk_accepted,
                                            catalog_persistent,
                                            repair_missing_game_marker)) {
    case core::PersistentMarkerState::inactive:
      return PersistentRuntimeState::inactive;
    case core::PersistentMarkerState::active:
      return PersistentRuntimeState::active;
    case core::PersistentMarkerState::invalid:
      return PersistentRuntimeState::invalid;
  }
  return PersistentRuntimeState::invalid;
}

DowngradeResult commit_persistent_runtime(const std::filesystem::path& game_root,
                                          bool risk_accepted,
                                          bool catalog_persistent) {
  const auto vault = core::resolve_vault_layout(game_root);
  if (!vault || !core::runtime_manifest_matches(*vault)) {
    return {ExitCode::backup_failed, false,
            L"The verified recovery-vault manifest is unavailable."};
  }
  if (!target_runtime_is_active(game_root)) {
    return {ExitCode::commit_failed, false,
            L"The persistent target runtime is not complete and verified."};
  }
  if (!core::commit_persistent_marker(*vault, game_root, risk_accepted,
                                      catalog_persistent)) {
    return {ExitCode::commit_failed, false,
            L"The persistent state could not be committed in the vault and transaction workspace."};
  }
  return {ExitCode::success, false,
          L"The persistent runtime and its recovery source were committed."};
}

bool preserve_recovery_conflict(const std::filesystem::path& game_root,
                                const std::filesystem::path& live,
                                std::string_view transaction_id) {
  const auto vault = core::resolve_vault_layout(game_root);
  return vault && core::preserve_conflict(*vault, live, transaction_id);
}

DowngradeResult clear_persistent_runtime(const std::filesystem::path& game_root) {
  const auto vault = core::resolve_vault_layout(game_root);
  if (!vault || !core::remove_persistent_marker(*vault, game_root)) {
    return {ExitCode::commit_failed, false,
            L"The persistent state marker could not be removed safely."};
  }
  return {ExitCode::success, false, L"The persistent state marker was removed."};
}

}  // namespace runtime_swapper
