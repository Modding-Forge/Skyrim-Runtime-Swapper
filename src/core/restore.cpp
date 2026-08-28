#include <runtime_swapper/downgrade.hpp>

#include "internal/runtime_transform.hpp"

namespace runtime_swapper {

DowngradeResult restore_runtime(const std::filesystem::path& game_root) {
  return core::transform_runtime(game_root, game_root / L"RuntimeSwap\\patches", false);
}

}  // namespace runtime_swapper
