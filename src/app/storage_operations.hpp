#pragma once

#include <runtime_swapper/exit_code.hpp>
#include <runtime_swapper/transaction_backend.hpp>
#include <runtime_swapper/recovery_lifecycle.hpp>

#include <filesystem>
#include <string>

namespace runtime_swapper::app {

struct InstallationOperationResult {
  ExitCode code{ExitCode::internal_error};
  BackendProbeResult backend;
  bool changed{};
  bool persistent{};
  bool runtime_changed{};
  bool content_catalog_changed{};
  bool creation_club_changed{};
  bool content_catalog_persistent{};
  RecoveryLifecycleState lifecycle_state{
      RecoveryLifecycleState::cleanup_pending};
  RecoveryLifecyclePhase lifecycle_phase{RecoveryLifecyclePhase::inspect};
  std::wstring technical_detail;
  std::wstring message;

  [[nodiscard]] bool success() const noexcept { return code == ExitCode::success; }
};

[[nodiscard]] InstallationOperationResult probe_installation_storage(
    const std::filesystem::path& game_root);

[[nodiscard]] InstallationOperationResult activate_session_target(
    const std::filesystem::path& game_root);
[[nodiscard]] InstallationOperationResult activate_persistent_target(
    const std::filesystem::path& game_root, bool risk_accepted);
[[nodiscard]] InstallationOperationResult restore_persistent_source(
    const std::filesystem::path& game_root);
[[nodiscard]] InstallationOperationResult recover_installation(
    const std::filesystem::path& game_root);

}  // namespace runtime_swapper::app
