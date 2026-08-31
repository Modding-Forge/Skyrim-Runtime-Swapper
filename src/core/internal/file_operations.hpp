#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace runtime_swapper::core {

struct ManagedFilePath {
  std::filesystem::path logical;
  std::filesystem::path effective;
  bool redirected{};
};

[[nodiscard]] std::filesystem::path utf8_path(std::string_view value);

[[nodiscard]] bool hash_matches(const std::filesystem::path& file,
                                std::string_view expected);

[[nodiscard]] std::wstring quote_path(const std::filesystem::path& path);

[[nodiscard]] bool has_minimum_free_space(const std::filesystem::path& root,
                                          std::uint64_t required_bytes);

// Resolves a managed runtime file without allowing it to escape the locked
// installation. A final file link is accepted only when its canonical
// regular-file target remains inside the same installation and filesystem.
// The symlink itself is never replaced; transactions operate on effective.
[[nodiscard]] std::optional<ManagedFilePath> resolve_managed_file(
    const std::filesystem::path& game_root,
    const std::filesystem::path& relative_file,
    std::wstring* error_message = nullptr);

// Revalidates a previously resolved mapping immediately before mutation.
[[nodiscard]] bool managed_file_mapping_matches(
    const std::filesystem::path& game_root,
    const ManagedFilePath& expected) noexcept;

}  // namespace runtime_swapper::core
