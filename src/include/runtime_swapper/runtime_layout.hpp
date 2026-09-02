#pragma once

#include <runtime_swapper/patch_plan.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace runtime_swapper {

enum class RuntimeLayout {
  standard,
  skse_launcher_alias,
  without_beafarmer,
  skse_launcher_alias_without_beafarmer,
  invalid,
};

[[nodiscard]] RuntimeLayout detect_runtime_layout(
    const std::filesystem::path& game_root,
    std::wstring* error_message = nullptr) noexcept;
[[nodiscard]] bool runtime_layout_matches(
    const std::filesystem::path& game_root, RuntimeLayout expected) noexcept;
[[nodiscard]] bool patch_plan_entry_enabled(
    RuntimeLayout layout, const PatchPlanEntry& entry) noexcept;
[[nodiscard]] std::size_t active_patch_plan_size(RuntimeLayout layout) noexcept;
[[nodiscard]] std::string_view runtime_layout_name(RuntimeLayout layout) noexcept;

}  // namespace runtime_swapper
