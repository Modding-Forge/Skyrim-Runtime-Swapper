#pragma once

#include <runtime_swapper/exit_code.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace runtime_swapper {

enum class RecoveryLifecycleState : std::uint8_t {
  clean_source,
  preparing,
  target_active,
  restoring,
  source_verified,
  cleanup_pending,
  persistent,
};

enum class RecoveryLifecyclePhase : std::uint8_t {
  inspect,
  prepare,
  activate,
  restore,
  verify_source,
  detach_locator,
  delete_recovery,
  delete_installation_metadata,
  complete,
};

struct RecoveryLifecycleResult {
  ExitCode code{ExitCode::internal_error};
  RecoveryLifecycleState state{RecoveryLifecycleState::cleanup_pending};
  RecoveryLifecyclePhase phase{RecoveryLifecyclePhase::inspect};
  std::wstring technical_detail;

  [[nodiscard]] bool success() const noexcept {
    return code == ExitCode::success;
  }
};

[[nodiscard]] bool recovery_transition_allowed(
    RecoveryLifecycleState from, RecoveryLifecycleState to) noexcept;
[[nodiscard]] std::string_view recovery_state_name(
    RecoveryLifecycleState state) noexcept;

}  // namespace runtime_swapper
