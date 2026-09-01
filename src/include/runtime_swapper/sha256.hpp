#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace runtime_swapper {

[[nodiscard]] std::optional<std::string> sha256_file(const std::filesystem::path& file);
// Hashes the exact already-open regular file represented by an OS HANDLE or
// file descriptor. The caller retains ownership of the handle.
[[nodiscard]] std::optional<std::string> sha256_native_file(
    std::intptr_t native_handle);
[[nodiscard]] std::optional<std::string> sha256_bytes(std::span<const std::byte> bytes);
[[nodiscard]] std::optional<std::string> sha256_string(std::string_view bytes);

}  // namespace runtime_swapper
