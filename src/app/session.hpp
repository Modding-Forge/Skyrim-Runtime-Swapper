#pragma once

#include <runtime_swapper/exit_code.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace runtime_swapper::app {

class UniqueHandle;

struct SessionResult {
  ExitCode code{ExitCode::success};
  std::wstring message;

  [[nodiscard]] bool success() const noexcept { return code == ExitCode::success; }
};

[[nodiscard]] std::wstring operation_mutex_name();
[[nodiscard]] std::wstring session_complete_event_name();
[[nodiscard]] UniqueHandle acquire_transaction_lock(const CoordinationLockPath& lock_path);

[[nodiscard]] bool launch_session_watcher(
    const std::filesystem::path& helper, const std::filesystem::path& game_root,
    DWORD loader_process_id, bool restore_runtime_after_session,
    bool restore_content_catalog_after_session, bool restore_creation_club_after_session,
    std::wstring_view diagnostic_session_id, std::wstring_view diagnostic_parent_run_id);

[[nodiscard]] SessionResult watch_session_and_restore(const std::filesystem::path& game_root,
                                                      DWORD loader_process_id,
                                                      bool restore_runtime_after_session,
                                                      bool restore_content_catalog_after_session,
                                                      bool restore_creation_club_after_session,
                                                      const std::wstring& ready_event_name);

} // namespace runtime_swapper::app
