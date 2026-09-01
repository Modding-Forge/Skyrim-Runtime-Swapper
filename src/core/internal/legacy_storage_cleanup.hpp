#pragma once

#include <runtime_swapper/legacy_storage_plan.hpp>

#include <filesystem>
#include <span>
#include <string>

namespace runtime_swapper::core {

struct LegacyStorageCleanupResult {
  bool success{};
  bool changed{};
  std::wstring detail;
};

[[nodiscard]] LegacyStorageCleanupResult cleanup_legacy_installation_storage(
    const std::filesystem::path& game_root,
    std::span<const LegacyManagedFile> managed_files = legacy_managed_files);

}  // namespace runtime_swapper::core
