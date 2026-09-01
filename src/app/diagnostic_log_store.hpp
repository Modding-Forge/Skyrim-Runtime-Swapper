#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace runtime_swapper::app {

inline constexpr std::size_t retained_primary_diagnostic_sessions = 30;

[[nodiscard]] std::size_t diagnostic_log_retention_offset(std::string_view contents,
                                                          std::size_t primary_sessions_to_keep);

[[nodiscard]] bool append_diagnostic_log(const std::filesystem::path& path,
                                         std::string_view utf8_line, bool primary_session) noexcept;
[[nodiscard]] bool remove_legacy_diagnostic_log(
    const std::filesystem::path& path) noexcept;

} // namespace runtime_swapper::app
