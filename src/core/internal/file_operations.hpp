#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace runtime_swapper::core {

[[nodiscard]] std::filesystem::path utf8_path(std::string_view value);

[[nodiscard]] bool hash_matches(const std::filesystem::path& file,
                                std::string_view expected);

[[nodiscard]] std::wstring quote_path(const std::filesystem::path& path);

[[nodiscard]] bool has_minimum_free_space(const std::filesystem::path& root,
                                          std::uint64_t required_bytes);

}  // namespace runtime_swapper::core
