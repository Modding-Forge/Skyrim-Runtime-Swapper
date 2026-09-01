#pragma once

#include <runtime_swapper/transaction_backend.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace runtime_swapper {

[[nodiscard]] std::string locator_contents(
    std::string_view installation, const std::filesystem::path& vault,
    const VolumeIdentity& vault_volume);
[[nodiscard]] bool locator_matches(
    const std::filesystem::path& locator, std::string_view installation,
    const std::filesystem::path& vault, const VolumeIdentity& vault_volume);
[[nodiscard]] std::optional<std::filesystem::path> locator_vault_path(
    const std::filesystem::path& locator, std::string_view installation);
[[nodiscard]] bool vault_manifest_identity_matches(
    const std::filesystem::path& vault, std::string_view installation,
    const VolumeIdentity& target_volume, const VolumeIdentity& vault_volume);

[[nodiscard]] BackendProbeResult blocked(
    std::wstring technical, std::wstring message,
    VolumeIdentity target = {}, VolumeIdentity vault = {},
    std::filesystem::path vault_path = {}, std::string installation = {});
[[nodiscard]] BackendProbeResult attach_storage_paths(
    BackendProbeResult result, const std::filesystem::path& recovery_base,
    const std::filesystem::path& target_base, std::string_view installation);

}  // namespace runtime_swapper
