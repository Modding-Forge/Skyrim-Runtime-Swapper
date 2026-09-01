#pragma once

#include <filesystem>

namespace runtime_swapper {

// Uses a cheap identity/size check before comparing content. This deliberately
// avoids hashing because callers only need an equality decision.
[[nodiscard]] bool files_have_identical_content(
    const std::filesystem::path& left,
    const std::filesystem::path& right) noexcept;

// Accepts the canonical SKSE filename or Amethyst's byte-identical copy over
// SkyrimSELauncher.exe. No other renamed executable is treated as SKSE.
[[nodiscard]] bool is_skse_loader_entry_image(
    const std::filesystem::path& process_image) noexcept;

}  // namespace runtime_swapper
