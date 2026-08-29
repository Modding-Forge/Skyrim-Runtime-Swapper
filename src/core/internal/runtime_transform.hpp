#pragma once

#include <runtime_swapper/downgrade.hpp>

#include <filesystem>

namespace runtime_swapper::core {

[[nodiscard]] DowngradeResult transform_runtime(
    const std::filesystem::path& game_root, const std::filesystem::path& patch_root,
    bool to_target, bool recover_first = true);

[[nodiscard]] DowngradeResult recover_runtime_transaction(
    const std::filesystem::path& game_root, const std::filesystem::path& patch_root);

}  // namespace runtime_swapper::core
