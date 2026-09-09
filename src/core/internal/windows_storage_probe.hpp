#pragma once

#include <runtime_swapper/transaction_backend.hpp>

namespace runtime_swapper {

[[nodiscard]] bool windows_storage_directory_is_private(
    const std::filesystem::path& directory);

[[nodiscard]] BackendProbeResult probe_windows_storage(
    TransactionBackend& backend, const std::filesystem::path& managed_root,
    std::uint64_t required_vault_bytes, bool prepare_vault);

[[nodiscard]] MutationResult prepare_windows_coordination_lock(
    const CoordinationLockPath& resolved_lock);

}  // namespace runtime_swapper
