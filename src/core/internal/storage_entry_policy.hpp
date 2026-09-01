#pragma once

#include <filesystem>

namespace runtime_swapper::core {

[[nodiscard]] bool verified_regular_input(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] bool private_regular_file(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] bool private_directory(
    const std::filesystem::path& path) noexcept;

}  // namespace runtime_swapper::core
