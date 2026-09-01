#pragma once

#include <filesystem>
#include <string>

namespace runtime_swapper {

[[nodiscard]] std::wstring present_path(
    const std::filesystem::path& path) noexcept;

}  // namespace runtime_swapper
