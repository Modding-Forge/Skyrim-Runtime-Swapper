#pragma once

#include "storage_operations.hpp"

#include <string_view>

namespace runtime_swapper::app {

inline constexpr std::string_view restore_intent_name = "persistent-restore";
inline constexpr std::string_view restore_intent = "SRS-PERSISTENT-RESTORE-1\n";

// Requires the installation lock. Re-inspects live state for each invocation,
// including retries from a stale GUI and recovery of an existing intent.
[[nodiscard]] InstallationOperationResult finish_restore(
    const std::filesystem::path& game_root, BackendProbeResult backend);

}  // namespace runtime_swapper::app
