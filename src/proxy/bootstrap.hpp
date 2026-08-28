#pragma once

#include <windows.h>

namespace runtime_swapper::proxy {

void set_module(HMODULE module) noexcept;
[[nodiscard]] bool ensure_runtime_ready(const wchar_t* queried_file) noexcept;
[[nodiscard]] bool ensure_runtime_ready(const char* queried_file) noexcept;

}  // namespace runtime_swapper::proxy
