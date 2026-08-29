#pragma once

#include <runtime_swapper/exit_code.hpp>
#include <runtime_swapper/patch_plan.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace runtime_swapper::core {

struct SourceBackupResult {
  ExitCode code{ExitCode::internal_error};
  bool changed{};
  std::wstring message;

  [[nodiscard]] bool success() const noexcept { return code == ExitCode::success; }
};

[[nodiscard]] std::uint64_t required_source_backup_space(
    const std::filesystem::path& game_root);

[[nodiscard]] SourceBackupResult ensure_source_backups(
    const std::filesystem::path& game_root);

[[nodiscard]] bool has_verified_source_backup(
    const std::filesystem::path& game_root, const PatchPlanEntry& plan);

[[nodiscard]] bool restore_source_backup(const std::filesystem::path& game_root,
                                         const PatchPlanEntry& plan,
                                         const std::filesystem::path& live);

}  // namespace runtime_swapper::core
