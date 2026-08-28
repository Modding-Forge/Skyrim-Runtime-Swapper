#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace runtime_swapper {

[[nodiscard]] std::optional<std::string> sha256_file(const std::filesystem::path& file);

}  // namespace runtime_swapper
