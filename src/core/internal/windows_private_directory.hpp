#pragma once

#include <windows.h>
#include <filesystem>
#include <string>
#include <system_error>

namespace runtime_swapper {
// Only new directories receive explicit ownership. Existing entries are untouched.
[[nodiscard]] bool create_windows_private_directories(
    const std::filesystem::path& path, PSID user, std::error_code& error);
[[nodiscard]] std::wstring windows_security_diagnostic(
    const std::filesystem::path& path, PSID user);
}
