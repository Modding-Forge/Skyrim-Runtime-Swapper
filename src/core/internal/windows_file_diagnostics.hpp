#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace runtime_swapper::core {

// Observational only. The owning RAII wrapper unregisters before CloseHandle;
// moving ownership must keep the registration until the final close.
void track_file_handle(HANDLE handle, const std::filesystem::path& path,
                       DWORD access, DWORD sharing, const wchar_t* owner) noexcept;
void untrack_file_handle(HANDLE handle) noexcept;

// Called only after a sharing/lock violation, before rollback closes handles.
// Neither closes another handle nor requests Restart Manager shutdown/restart.
[[nodiscard]] std::wstring file_lock_diagnostics(
    const std::filesystem::path& path, DWORD access, DWORD sharing) noexcept;

}  // namespace runtime_swapper::core
