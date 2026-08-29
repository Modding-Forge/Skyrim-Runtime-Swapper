#pragma once

#include <filesystem>
#include <string>

namespace runtime_swapper::app {

enum class FixedRuntimeState { inactive, active, invalid };

struct FixedRuntimeResult {
  bool success{};
  std::wstring message;
};

[[nodiscard]] FixedRuntimeState inspect_fixed_runtime(
    const std::filesystem::path& game_root) noexcept;

[[nodiscard]] FixedRuntimeResult enable_fixed_runtime(
    const std::filesystem::path& game_root);

[[nodiscard]] FixedRuntimeResult disable_fixed_runtime(
    const std::filesystem::path& game_root);

}  // namespace runtime_swapper::app
