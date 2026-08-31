#pragma once

#include <runtime_swapper/exit_code.hpp>

#include <windows.h>

#include <string>

namespace runtime_swapper::app {

void log_diagnostic(const std::wstring& message) noexcept;
[[nodiscard]] bool copy_diagnostic_logs() noexcept;

int finish(ExitCode code, const std::wstring& message, UINT icon, bool quiet = false);

}  // namespace runtime_swapper::app
