#include <runtime_swapper/downgrade.hpp>

#include "internal/runtime_transform.hpp"

namespace runtime_swapper {

DowngradeResult downgrade_runtime(const std::filesystem::path& game_root,
                                  const std::filesystem::path& patch_root) {
  return core::transform_runtime(game_root, patch_root, true);
}

DowngradeResult downgrade_runtime_after_recovery(
    const std::filesystem::path& game_root, const std::filesystem::path& patch_root) {
  return core::transform_runtime(game_root, patch_root, true, false);
}

}  // namespace runtime_swapper
