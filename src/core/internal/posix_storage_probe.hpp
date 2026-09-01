#pragma once

#include <runtime_swapper/transaction_backend.hpp>

#include <optional>

namespace runtime_swapper {

[[nodiscard]] BackendProbeResult probe_posix_storage(
    TransactionBackend& backend, const std::filesystem::path& managed_root,
    std::uint64_t required_vault_bytes, bool prepare_vault);

[[nodiscard]] std::optional<std::uint64_t> posix_mount_id(
    const std::filesystem::path& path) noexcept;

[[nodiscard]] std::optional<std::filesystem::path>
posix_existing_directory_ancestor(std::filesystem::path path);
[[nodiscard]] bool posix_directory_controlled_by_user(
    const std::filesystem::path& directory);
[[nodiscard]] bool posix_ensure_directory_hierarchy(
    const std::filesystem::path& anchor, const std::filesystem::path& target,
    bool private_children);
[[nodiscard]] bool posix_secure_private_hierarchy(
    const std::filesystem::path& anchor, const std::filesystem::path& target);

}  // namespace runtime_swapper

