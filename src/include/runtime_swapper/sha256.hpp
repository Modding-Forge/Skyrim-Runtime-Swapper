#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace runtime_swapper {

[[nodiscard]] std::optional<std::string> sha256_file(const std::filesystem::path& file);
[[nodiscard]] std::optional<std::string> sha256_bytes(std::span<const std::byte> bytes);
[[nodiscard]] std::optional<std::string> sha256_string(std::string_view bytes);

}  // namespace runtime_swapper
