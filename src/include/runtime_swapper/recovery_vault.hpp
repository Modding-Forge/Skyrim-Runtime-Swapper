#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace runtime_swapper {

[[nodiscard]] bool commit_recovery_file(const std::filesystem::path& game_root,
                                        const std::filesystem::path& source,
                                        std::string_view sha256,
                                        std::uint64_t expected_size);
[[nodiscard]] bool restore_recovery_file(const std::filesystem::path& game_root,
                                         std::string_view sha256,
                                         std::uint64_t expected_size,
                                         const std::filesystem::path& destination);
[[nodiscard]] bool recovery_file_available(const std::filesystem::path& game_root,
                                           std::string_view sha256,
                                           std::uint64_t expected_size);

[[nodiscard]] bool write_recovery_metadata(const std::filesystem::path& game_root,
                                           std::string_view name,
                                           std::string_view contents);
[[nodiscard]] std::optional<std::string> read_recovery_metadata(
    const std::filesystem::path& game_root, std::string_view name);
[[nodiscard]] bool remove_recovery_metadata(const std::filesystem::path& game_root,
                                            std::string_view name);

}  // namespace runtime_swapper
