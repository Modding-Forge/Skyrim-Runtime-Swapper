#include "session.hpp"

#include "content_catalog.hpp"
#include "creation_club.hpp"
#include "diagnostics.hpp"
#include "runtime_labels.hpp"
#include "storage_operations.hpp"
#include "unique_handle.hpp"
#include "wine_sidecar.hpp"

#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/transaction_backend.hpp>

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

UniqueHandle acquire_transaction_lock(
    const CoordinationLockPath& resolved_lock) {
  const auto& lock_path = resolved_lock.value;
  if (lock_path.empty() || !lock_path.is_absolute() ||
      !managed_path_is_safe(lock_path.parent_path())) {
    return {};
  }
  std::error_code error;
  std::filesystem::create_directories(lock_path.parent_path(), error);
  if (error || !managed_path_is_safe(lock_path.parent_path())) return {};
  UniqueHandle lock(CreateFileW(
      lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
      FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_WRITE_THROUGH |
          FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!lock) return {};
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_STANDARD_INFO standard{};
  if (!GetFileInformationByHandleEx(lock.get(), FileAttributeTagInfo,
                                    &attributes, sizeof(attributes)) ||
      !GetFileInformationByHandleEx(lock.get(), FileStandardInfo, &standard,
                                    sizeof(standard)) ||
      (attributes.FileAttributes &
       (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
      standard.NumberOfLinks != 1) {
    return {};
  }
  return lock;
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

  UniqueHandle transaction_lock;
  if (!is_wine_environment()) {
    const auto probed = probe_installation_storage(game_root);
    if (!probed.success()) {
      return {probed.code, probed.message};
    }
    transaction_lock = acquire_transaction_lock(
        probed.backend.coordination_lock);
    if (!transaction_lock) {
      return {ExitCode::another_instance_failed,
              L"The watcher could not acquire the durable runtime transaction lock."};
    }
  }

  if (is_wine_environment() &&
      (restore_runtime_after_session || restore_content_catalog_after_session ||
       restore_creation_club_after_session)) {
    const auto restored =
        run_wine_sidecar(WineSidecarOperation::recover, game_root);
    log_diagnostic(L"Native sidecar session recovery: " + restored.message);
    return restored.success()
               ? SessionResult{}
               : SessionResult{restored.code, restored.message};
  }

  if (restore_runtime_after_session || restore_content_catalog_after_session ||
      restore_creation_club_after_session) {
    const auto restored = recover_installation(game_root);
    log_diagnostic(L"Watcher installation restore: " + restored.message);
    if (!restored.success()) return {restored.code, restored.message};
  }
  return {};
}

}  // namespace runtime_swapper::app
