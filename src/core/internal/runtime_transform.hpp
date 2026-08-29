#pragma once

#include <runtime_swapper/downgrade.hpp>

#include <filesystem>

namespace runtime_swapper::core {

[[nodiscard]] DowngradeResult transform_runtime(
    const std::filesystem::path& game_root, const std::filesystem::path& patch_root,
    bool to_target, bool recover_first = true);

[[nodiscard]] DowngradeResult recover_runtime_transaction(
    const std::filesystem::path& game_root, const std::filesystem::path& patch_root);

[[nodiscard]] bool target_runtime_is_active_internal(
    const std::filesystem::path& game_root) noexcept;

[[nodiscard]] DowngradeResult finalize_fixed_target_runtime_internal(
    const std::filesystem::path& game_root);

}  // namespace runtime_swapper::core
