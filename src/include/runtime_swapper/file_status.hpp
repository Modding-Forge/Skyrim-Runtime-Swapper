#pragma once

#include <filesystem>
#include <system_error>

namespace runtime_swapper {

enum class RegularFileStatus { missing, regular, not_regular, error };

[[nodiscard]] inline RegularFileStatus inspect_regular_file(
    const std::filesystem::path& path, std::error_code& error) noexcept {
  const auto status = std::filesystem::status(path, error);
  if (status.type() == std::filesystem::file_type::not_found) {
    error.clear();
    return RegularFileStatus::missing;
  }
  if (error) {
    return RegularFileStatus::error;
  }
  return status.type() == std::filesystem::file_type::regular
             ? RegularFileStatus::regular
             : RegularFileStatus::not_regular;
}

}  // namespace runtime_swapper
