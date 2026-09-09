#pragma once

#include <runtime_swapper/transaction_backend.hpp>

namespace runtime_swapper {

// Only resolver-derived SRS directories are eligible. No game files, arbitrary
// locator targets, recursive traversal, or recovery-content deletion is allowed.
[[nodiscard]] bool windows_storage_directories_need_repair(
    const BackendProbeResult& probe) noexcept;
[[nodiscard]] MutationResult repair_windows_storage_directories(
    const BackendProbeResult& probe) noexcept;

}  // namespace runtime_swapper
