#include <runtime_swapper/downgrade.hpp>

#include "internal/runtime_transform.hpp"

namespace runtime_swapper {

DowngradeResult restore_runtime(const std::filesystem::path& game_root) {
  return core::transform_runtime(game_root, game_root / L"RuntimeSwap\\patches", false);
}

DowngradeResult recover_runtime(const std::filesystem::path& game_root) {
  return core::recover_runtime_transaction(game_root,
                                           game_root / L"RuntimeSwap\\patches");
}

bool target_runtime_is_active(const std::filesystem::path& game_root) noexcept {
  return core::target_runtime_is_active_internal(game_root);
}

DowngradeResult finalize_fixed_target_runtime(
    const std::filesystem::path& game_root) {
  return core::finalize_fixed_target_runtime_internal(game_root);
}

}  // namespace runtime_swapper
