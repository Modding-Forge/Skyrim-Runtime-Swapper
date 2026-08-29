#include "session.hpp"

#include "content_catalog.hpp"
#include "creation_club.hpp"
#include "diagnostics.hpp"
#include "runtime_labels.hpp"
#include "unique_handle.hpp"

#include <runtime_swapper/downgrade.hpp>

#include <windows.h>
#include <tlhelp32.h>

#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace runtime_swapper::app {
namespace {

[[nodiscard]] std::wstring watcher_ready_event_name(DWORD loader_process_id) {
  return operation_mutex_name() + L"-WatcherReady-" + std::to_wstring(loader_process_id);
}

[[nodiscard]] bool paths_equal(const std::filesystem::path& left,
                               const std::filesystem::path& right) {
  const auto left_text = left.lexically_normal().wstring();
  const auto right_text = right.lexically_normal().wstring();
  return CompareStringOrdinal(left_text.data(), static_cast<int>(left_text.size()),
                              right_text.data(), static_cast<int>(right_text.size()), TRUE) ==
         CSTR_EQUAL;
}

[[nodiscard]] UniqueHandle find_process_by_image(
    const std::filesystem::path& expected_image) {
  UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (!snapshot) return {};

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot.get(), &entry)) return {};
  do {
    if (entry.th32ProcessID == GetCurrentProcessId()) continue;
    UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                     entry.th32ProcessID));
    if (!process) continue;
    std::vector<wchar_t> image(32768);
    DWORD size = static_cast<DWORD>(image.size());
    if (QueryFullProcessImageNameW(process.get(), 0, image.data(), &size) &&
        paths_equal(std::filesystem::path(std::wstring_view(image.data(), size)), expected_image)) {
      return process;
    }
  } while (Process32NextW(snapshot.get(), &entry));
  return {};
}

}  // namespace

std::wstring operation_mutex_name() {
  return L"Local\\SkyrimRuntimeSwapper-" + source_version() + L"-to-" + target_version();
}

std::wstring session_complete_event_name() {
  return operation_mutex_name() + L"-SessionComplete";
}

bool launch_session_watcher(const std::filesystem::path& helper,
                            const std::filesystem::path& game_root,
                            DWORD loader_process_id,
                            bool restore_runtime_after_session,
                            bool restore_content_catalog_after_session,
                            bool restore_creation_club_after_session) {
  const auto ready_event_name = watcher_ready_event_name(loader_process_id);
  UniqueHandle ready_event(CreateEventW(nullptr, TRUE, FALSE, ready_event_name.c_str()));
  if (!ready_event) return false;
  ResetEvent(ready_event.get());

  std::wstring command = L"\"" + helper.wstring() + L"\" --watch --game-root \"" +
                         game_root.wstring() + L"\" --loader-pid " +
                         std::to_wstring(loader_process_id) + L" --ready-event \"" +
                         ready_event_name + L"\"";
  if (restore_runtime_after_session) command += L" --restore-runtime";
  if (restore_content_catalog_after_session) command += L" --restore-content-catalog";
  if (restore_creation_club_after_session) command += L" --restore-creation-club";

  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process_info{};
  if (!CreateProcessW(helper.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                      game_root.c_str(), &startup, &process_info)) {
    return false;
  }
  UniqueHandle process(process_info.hProcess);
  UniqueHandle thread(process_info.hThread);

  const HANDLE wait_handles[] = {ready_event.get(), process.get()};
  const DWORD ready_result = WaitForMultipleObjects(static_cast<DWORD>(std::size(wait_handles)),
                                                     wait_handles, FALSE, 30'000);
  if (ready_result == WAIT_OBJECT_0) return true;

  TerminateProcess(process.get(), static_cast<UINT>(ExitCode::watcher_start_failed));
  WaitForSingleObject(process.get(), 5'000);
  UniqueHandle complete_event(
      CreateEventW(nullptr, TRUE, TRUE, session_complete_event_name().c_str()));
  if (complete_event) SetEvent(complete_event.get());
  return false;
}

SessionResult watch_session_and_restore(const std::filesystem::path& game_root,
                                        DWORD loader_process_id,
                                        bool restore_runtime_after_session,
                                        bool restore_content_catalog_after_session,
                                        bool restore_creation_club_after_session,
                                        const std::wstring& ready_event_name) {
  UniqueHandle ready_event(OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_event_name.c_str()));
  UniqueHandle complete_event(
      CreateEventW(nullptr, TRUE, TRUE, session_complete_event_name().c_str()));
  if (!ready_event || !complete_event || !ResetEvent(complete_event.get()) ||
      !SetEvent(ready_event.get())) {
    if (complete_event) SetEvent(complete_event.get());
    return {ExitCode::watcher_start_failed,
            L"The watcher could not establish the session barrier."};
  }

  struct SessionCompletionSignal {
    HANDLE event{};
    ~SessionCompletionSignal() {
      if (event != nullptr) SetEvent(event);
    }
  } completion_signal{complete_event.get()};

  UniqueHandle loader(OpenProcess(SYNCHRONIZE, FALSE, loader_process_id));
  const auto skyrim = game_root / L"SkyrimSE.exe";
  const ULONGLONG absolute_deadline = GetTickCount64() + 60'000;
  ULONGLONG loader_exit_deadline{};
  UniqueHandle game_process;

  while (GetTickCount64() < absolute_deadline) {
    game_process = find_process_by_image(skyrim);
    if (game_process) break;

    if (loader && WaitForSingleObject(loader.get(), 0) == WAIT_OBJECT_0 &&
        loader_exit_deadline == 0) {
      loader_exit_deadline = GetTickCount64() + 5'000;
    }
    if (loader_exit_deadline != 0 && GetTickCount64() >= loader_exit_deadline) break;
    Sleep(50);
  }

  if (game_process) {
    WaitForSingleObject(game_process.get(), INFINITE);
    game_process.reset();
    Sleep(100);
  }

  UniqueHandle mutex(CreateMutexW(nullptr, FALSE, operation_mutex_name().c_str()));
  if (!mutex) {
    return {ExitCode::another_instance_failed,
            L"The watcher could not create the runtime operation lock."};
  }
  const DWORD wait_result = WaitForSingleObject(mutex.get(), 5 * 60 * 1000);
  if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
    return {ExitCode::another_instance_failed,
            L"The watcher could not restore Skyrim before the timeout."};
  }

  struct MutexRelease {
    HANDLE mutex{};
    ~MutexRelease() {
      if (mutex != nullptr) ReleaseMutex(mutex);
    }
  } mutex_release{mutex.get()};

  const auto transaction_lock_path =
      game_root / L".skyrim-runtime-swapper" / L"transaction.lock";
  std::error_code lock_error;
  std::filesystem::create_directories(transaction_lock_path.parent_path(), lock_error);
  UniqueHandle transaction_lock(
      lock_error ? INVALID_HANDLE_VALUE
                 : CreateFileW(transaction_lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_WRITE_THROUGH, nullptr));
  if (!transaction_lock) {
    return {ExitCode::another_instance_failed,
            L"The watcher could not acquire the durable runtime transaction lock."};
  }

  if (restore_runtime_after_session) {
    const auto restored = restore_runtime(game_root);
    log_diagnostic(L"Watcher runtime restore: " + restored.message);
    if (!restored.success()) return {restored.code, restored.message};
  }
  if (restore_creation_club_after_session) {
    const auto restored = recover_creation_club_content(game_root);
    log_diagnostic(restored.success
                       ? L"Watcher Creation Club restore: complete"
                       : L"Watcher Creation Club restore failed: " + restored.message);
    if (!restored.success) {
      return {ExitCode::creation_club_cleanup_failed,
              L"Creation Club content could not be restored after the game session."};
    }
  }
  if (restore_content_catalog_after_session) {
    const auto restored = restore_content_catalog(game_root);
    log_diagnostic(restored.success ? L"Watcher ContentCatalog restore: complete"
                                    : L"Watcher ContentCatalog restore failed: " +
                                          restored.message);
    if (!restored.success) {
      return {ExitCode::content_catalog_cleanup_failed,
              L"ContentCatalog.txt could not be restored after the game session."};
    }
  }
  return {};
}

}  // namespace runtime_swapper::app
