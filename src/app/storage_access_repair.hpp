#pragma once

#include <runtime_swapper/transaction_backend.hpp>

#include <filesystem>
#include <string>

namespace runtime_swapper::app {

enum class StorageAccessRepairResult {
  not_needed,
  cancelled,
  failed,
  succeeded,
};

// Only existing, plain SRS directories with incorrect ownership or private
// permissions are eligible. Resolver or identity failures are not repairable.
[[nodiscard]] bool windows_storage_access_repair_needed(
    const BackendProbeResult& probe) noexcept;

// Starts this trusted helper once through the Windows UAC consent dialog, then
// waits for its result.  The elevated invocation re-derives every path from the
// game root; it never accepts a storage path from the command line.
[[nodiscard]] StorageAccessRepairResult request_windows_storage_access_repair(
    const std::filesystem::path& helper_path,
    const std::filesystem::path& game_root) noexcept;

// Called only by the elevated helper command.  It changes ownership and the
// private DACL of existing SRS storage and vault directories. Game files and
// recovery contents are never changed or removed by this repair plan.
[[nodiscard]] StorageAccessRepairResult repair_windows_storage_access(
    const std::filesystem::path& game_root, std::wstring* detail = nullptr) noexcept;

}  // namespace runtime_swapper::app
