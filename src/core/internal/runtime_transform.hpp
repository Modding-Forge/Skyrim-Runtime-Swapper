#pragma once

#include <runtime_swapper/downgrade.hpp>

#include <filesystem>

namespace runtime_swapper::core {

[[nodiscard]] DowngradeResult transform_runtime(
    const std::filesystem::path& game_root, const std::filesystem::path& patch_root,
    bool to_target, bool recover_first = true, bool risk_accepted = false);

[[nodiscard]] DowngradeResult recover_runtime_transaction(
    const std::filesystem::path& game_root, const std::filesystem::path& patch_root);

[[nodiscard]] DowngradeResult recover_to_source_internal(
    const std::filesystem::path& game_root, const std::filesystem::path& patch_root,
    bool restore_clean_target = false);

void clean_runtime_transaction_best_effort(
    const std::filesystem::path& game_root,
    const std::filesystem::path& transaction_root) noexcept;

[[nodiscard]] bool target_runtime_is_active_internal(
    const std::filesystem::path& game_root) noexcept;
[[nodiscard]] bool source_runtime_is_active_internal(
    const std::filesystem::path& game_root) noexcept;

[[nodiscard]] DowngradeResult finalize_fixed_target_runtime_internal(
    const std::filesystem::path& game_root);

}  // namespace runtime_swapper::core
