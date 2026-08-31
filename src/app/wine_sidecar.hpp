#pragma once

#include "storage_operations.hpp"

#include <filesystem>

namespace runtime_swapper::app {

enum class WineSidecarOperation : unsigned short {
  probe = 1,
  recover = 2,
  activate_session = 3,
  activate_persistent = 4,
  restore_persistent = 5,
  prepare_launch = 6,
};

[[nodiscard]] InstallationOperationResult run_wine_sidecar(
    WineSidecarOperation operation, const std::filesystem::path& game_root,
    bool risk_accepted = false, bool allow_persistent = false);

}  // namespace runtime_swapper::app
