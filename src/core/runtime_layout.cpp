#include <runtime_swapper/runtime_layout.hpp>

#include "internal/file_operations.hpp"

#include <runtime_swapper/file_identity.hpp>

#include <algorithm>

namespace runtime_swapper {
namespace {

constexpr std::string_view launcher_name = "SkyrimSELauncher.exe";

}  // namespace

RuntimeLayout detect_runtime_layout(
    const std::filesystem::path& game_root) noexcept {
  try {
    const auto launcher = core::resolve_managed_file(game_root, launcher_name);
    // The launcher is managed by the runtime transaction and must therefore
    // remain inside the installation. The SKSE loader is read-only input to
    // this classification, however, and mod managers commonly expose it as a
    // final symlink into their deployment store. Compare the opened object at
    // its logical path without granting that external target write authority.
    const auto skse_loader = game_root / "skse64_loader.exe";
    if (launcher &&
        files_have_identical_content(launcher->effective, skse_loader)) {
      return RuntimeLayout::skse_launcher_alias;
    }
  } catch (...) {
  }
  return RuntimeLayout::standard;
}

bool runtime_layout_matches(const std::filesystem::path& game_root,
                            RuntimeLayout expected) noexcept {
  return detect_runtime_layout(game_root) == expected;
}

bool patch_plan_entry_enabled(RuntimeLayout layout,
                              const PatchPlanEntry& entry) noexcept {
  return layout != RuntimeLayout::skse_launcher_alias ||
         entry.relative_file != launcher_name;
}

std::size_t active_patch_plan_size(RuntimeLayout layout) noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      patch_plan, [layout](const PatchPlanEntry& entry) {
        return patch_plan_entry_enabled(layout, entry);
      }));
}

std::string_view runtime_layout_name(RuntimeLayout layout) noexcept {
  switch (layout) {
    case RuntimeLayout::standard:
      return "standard";
    case RuntimeLayout::skse_launcher_alias:
      return "skse-launcher-alias";
  }
  return "unknown";
}

}  // namespace runtime_swapper
