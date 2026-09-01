#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <runtime_swapper/transaction_backend.hpp>

namespace runtime_swapper::app {

enum class StoragePathRole {
  recovery_vault,
  target_cache,
  coordination_lock,
  transaction_work,
};

[[nodiscard]] std::wstring display_path(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] std::wstring display_reported_path(
    const ReportedPath& path) noexcept;
[[nodiscard]] std::wstring display_storage_path(
    const BackendProbeResult& backend, StoragePathRole role) noexcept;
[[nodiscard]] std::wstring quote_display_path(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] std::wstring quote_windows_command_argument(
    std::wstring_view value);

}  // namespace runtime_swapper::app
