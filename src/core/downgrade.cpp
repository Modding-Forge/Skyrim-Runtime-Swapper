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

DowngradeResult downgrade_runtime_persistent_after_recovery(
    const std::filesystem::path& game_root,
    const std::filesystem::path& patch_root, bool risk_accepted) {
  return core::transform_runtime(game_root, patch_root, true, false, risk_accepted);
}

}  // namespace runtime_swapper
