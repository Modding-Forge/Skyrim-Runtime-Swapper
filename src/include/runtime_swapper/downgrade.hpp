#pragma once

#include <runtime_swapper/exit_code.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace runtime_swapper {

struct DowngradeResult {
  ExitCode code{ExitCode::internal_error};
  bool changed_files{};
  std::wstring message;

  [[nodiscard]] bool success() const noexcept { return code == ExitCode::success; }
};

enum class PersistentRuntimeState { inactive, active, invalid };

[[nodiscard]] DowngradeResult downgrade_runtime(const std::filesystem::path& game_root,
                                                const std::filesystem::path& patch_root);

[[nodiscard]] DowngradeResult downgrade_runtime_after_recovery(
    const std::filesystem::path& game_root, const std::filesystem::path& patch_root);

[[nodiscard]] DowngradeResult downgrade_runtime_persistent_after_recovery(
    const std::filesystem::path& game_root, const std::filesystem::path& patch_root,
    bool risk_accepted);

[[nodiscard]] DowngradeResult restore_runtime(const std::filesystem::path& game_root);

[[nodiscard]] DowngradeResult recover_runtime(const std::filesystem::path& game_root);

[[nodiscard]] bool target_runtime_is_active(
    const std::filesystem::path& game_root) noexcept;

[[nodiscard]] DowngradeResult finalize_fixed_target_runtime(
    const std::filesystem::path& game_root);

[[nodiscard]] PersistentRuntimeState inspect_persistent_runtime(
    const std::filesystem::path& game_root, bool* risk_accepted = nullptr,
    bool* catalog_persistent = nullptr,
    bool repair_missing_game_marker = true);
[[nodiscard]] DowngradeResult commit_persistent_runtime(
    const std::filesystem::path& game_root, bool risk_accepted,
    bool catalog_persistent = false);
[[nodiscard]] DowngradeResult clear_persistent_runtime(
    const std::filesystem::path& game_root);

[[nodiscard]] bool preserve_recovery_conflict(
    const std::filesystem::path& game_root, const std::filesystem::path& live,
    std::string_view transaction_id);

}  // namespace runtime_swapper
