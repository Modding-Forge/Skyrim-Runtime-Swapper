#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/session_gate.hpp>
#include <runtime_swapper/session_plan.hpp>

#include <atomic>
#include <filesystem>
#include <process.h>
#include <system_error>
#include <thread>

int main() {
  using runtime_swapper::RuntimeVersion;

  static_assert(runtime_swapper::source_runtime > runtime_swapper::target_runtime);

  if (runtime_swapper::source_runtime.to_string() != runtime_swapper::source_version_label) {
    return 1;
  }
  if (runtime_swapper::target_runtime.to_string() != runtime_swapper::target_version_label) {
    return 2;
  }
  if (RuntimeVersion{1, 6, 1170, 1}.to_string() != L"1.6.1170.1") {
    return 3;
  }

  constexpr auto swapped_runtime = runtime_swapper::make_session_plan(true, true, true);
  static_assert(swapped_runtime.start_watcher);
  static_assert(swapped_runtime.restore_runtime_after_session);
  static_assert(swapped_runtime.restore_content_catalog_after_session);

  constexpr auto existing_target = runtime_swapper::make_session_plan(true, false, true);
  static_assert(existing_target.start_watcher);
  static_assert(!existing_target.restore_runtime_after_session);
  static_assert(existing_target.restore_content_catalog_after_session);

  constexpr auto untouched_target = runtime_swapper::make_session_plan(true, false, false);
  static_assert(!untouched_target.start_watcher);
  static_assert(!untouched_target.restore_runtime_after_session);
  static_assert(!untouched_target.restore_content_catalog_after_session);

  constexpr auto swapped_without_catalog = runtime_swapper::make_session_plan(true, true, false);
  static_assert(swapped_without_catalog.start_watcher);
  static_assert(swapped_without_catalog.restore_runtime_after_session);
  static_assert(!swapped_without_catalog.restore_content_catalog_after_session);

  std::error_code filesystem_error;
  const auto missing_catalog = std::filesystem::temp_directory_path() /
                               (L"skyrim-runtime-swapper-missing-content-catalog-" +
                                std::to_wstring(_getpid())) /
                               L"ContentCatalog.txt";
  const auto missing_status =
      runtime_swapper::inspect_regular_file(missing_catalog, filesystem_error);
  if (missing_status != runtime_swapper::RegularFileStatus::missing || filesystem_error) {
    return 4;
  }

  HANDLE complete_event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  HANDLE operation_mutex = CreateMutexW(nullptr, FALSE, nullptr);
  if (complete_event == nullptr || operation_mutex == nullptr) {
    if (complete_event != nullptr) CloseHandle(complete_event);
    if (operation_mutex != nullptr) CloseHandle(operation_mutex);
    return 5;
  }
  const bool immediate_lock = runtime_swapper::wait_for_inactive_session_and_lock(
      complete_event, operation_mutex, 1'000);
  if (immediate_lock) ReleaseMutex(operation_mutex);
  CloseHandle(operation_mutex);
  CloseHandle(complete_event);
  if (!immediate_lock) return 6;

  complete_event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  operation_mutex = CreateMutexW(nullptr, TRUE, nullptr);
  if (complete_event == nullptr || operation_mutex == nullptr) {
    if (complete_event != nullptr) CloseHandle(complete_event);
    if (operation_mutex != nullptr) CloseHandle(operation_mutex);
    return 7;
  }

  std::atomic_bool worker_started{};
  std::atomic_bool worker_finished{};
  std::atomic_bool worker_locked{};
  std::thread competing_start([&] {
    worker_started = true;
    worker_locked = runtime_swapper::wait_for_inactive_session_and_lock(
        complete_event, operation_mutex, 2'000);
    if (worker_locked) ReleaseMutex(operation_mutex);
    worker_finished = true;
  });
  while (!worker_started) Sleep(1);
  Sleep(50);
  ResetEvent(complete_event);
  ReleaseMutex(operation_mutex);
  Sleep(50);
  const bool bypassed_active_session = worker_finished;
  SetEvent(complete_event);
  competing_start.join();
  CloseHandle(operation_mutex);
  CloseHandle(complete_event);
  if (bypassed_active_session || !worker_locked) return 8;
  return 0;
}
