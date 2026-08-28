#pragma once

#include <runtime_swapper/downgrade.hpp>

#include <filesystem>

namespace runtime_swapper::core {

[[nodiscard]] DowngradeResult transform_runtime(
    const std::filesystem::path& game_root, const std::filesystem::path& patch_root,
    bool to_target);

}  // namespace runtime_swapper::core
