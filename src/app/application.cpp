#include "application.hpp"

#include "command_line.hpp"
#include "content_catalog.hpp"
#include "diagnostics.hpp"
#include "runtime_labels.hpp"
#include "runtime_version_reader.hpp"
#include "session.hpp"
#include "unique_handle.hpp"

#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/exit_code.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/session_gate.hpp>
#include <runtime_swapper/session_plan.hpp>

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace runtime_swapper::app {
namespace {

class MutexLock {
 public:
  explicit MutexLock(HANDLE mutex) noexcept : mutex_(mutex) {}
  ~MutexLock() { unlock(); }

  MutexLock(const MutexLock&) = delete;
  MutexLock& operator=(const MutexLock&) = delete;

  void unlock() noexcept {
    if (mutex_ == nullptr) return;
    ReleaseMutex(mutex_);
    mutex_ = nullptr;
  }

 private:
  HANDLE mutex_{};
};

[[nodiscard]] int run_watcher(const CommandLineOptions& options) {
  if (!options.loader_process_id || !options.ready_event_name) {
    return finish(ExitCode::invalid_arguments,
                  L"The watcher did not receive valid session information.", MB_ICONERROR,
                  options.quiet);
  }
  const auto result = watch_session_and_restore(
      *options.game_root, *options.loader_process_id, options.restore_runtime_after_session,
      options.restore_content_catalog_after_session, *options.ready_event_name);
  if (!result.success()) return finish(result.code, result.message, MB_ICONERROR, options.quiet);
  return static_cast<int>(ExitCode::success);
}

}  // namespace

int run(int argc, wchar_t** argv) {
  const auto options = parse_command_line(argc, argv);
  if (!options.game_root) {
    return finish(ExitCode::invalid_arguments, L"The game directory was not specified.",
                  MB_ICONERROR, options.quiet);
  }
  if (options.watch) return run_watcher(options);

  UniqueHandle mutex(CreateMutexW(nullptr, FALSE, operation_mutex_name().c_str()));
  UniqueHandle session_complete_event(
      CreateEventW(nullptr, TRUE, TRUE, session_complete_event_name().c_str()));
  if (!mutex || !session_complete_event) {
    return finish(ExitCode::another_instance_failed,
                  L"The session synchronization objects could not be created.", MB_ICONERROR,
                  options.quiet);
  }
  if (!wait_for_inactive_session_and_lock(session_complete_event.get(), mutex.get(),
                                          5 * 60 * 1000)) {
    return finish(ExitCode::another_instance_failed,
                  L"The previous Skyrim session was not restored before the timeout.",
                  MB_ICONERROR, options.quiet);
  }
  MutexLock mutex_lock(mutex.get());

  const auto executable = *options.game_root / L"SkyrimSE.exe";
  if (!std::filesystem::is_regular_file(executable)) {
    mutex_lock.unlock();
    return finish(ExitCode::game_not_found,
                  L"SkyrimSE.exe was not found in the game directory.", MB_ICONERROR,
                  options.quiet);
  }

  const auto version = read_runtime_version(executable);
  if (!version) {
    mutex_lock.unlock();
    return finish(ExitCode::version_read_failed,
                  L"The installed Skyrim version could not be read.", MB_ICONERROR,
                  options.quiet);
  }
  if (*version != source_runtime && *version != target_runtime) {
    mutex_lock.unlock();
    return finish(ExitCode::unsupported_runtime,
                  L"Unsupported Skyrim version: " + version->to_string() +
                      L"\n\nNo files were changed.",
                  MB_ICONERROR, options.quiet);
  }

  const auto patch_root = *options.game_root / L"RuntimeSwap\\patches";
  const auto result = downgrade_runtime(*options.game_root, patch_root);
  ContentCatalogResult catalog_cleanup{};
  if (result.success()) {
    catalog_cleanup = remove_incompatible_content_catalog(*options.game_root);
  }

  const auto session_plan = make_session_plan(
      options.from_skse_loader, result.changed_files, catalog_cleanup.changed);
  bool watcher_started = true;
  std::optional<DowngradeResult> safety_restore;
  bool safety_catalog_restored = true;
  if (result.success() && catalog_cleanup.success && session_plan.start_watcher) {
    watcher_started =
        options.loader_process_id &&
        launch_session_watcher(options.helper_path, *options.game_root,
                               *options.loader_process_id,
                               session_plan.restore_runtime_after_session,
                               session_plan.restore_content_catalog_after_session);
  }
  if (result.success() && (!catalog_cleanup.success || !watcher_started)) {
    if (result.changed_files) safety_restore = restore_runtime(*options.game_root);
    if (catalog_cleanup.changed) {
      safety_catalog_restored = restore_content_catalog(*options.game_root).success;
    }
  }
  mutex_lock.unlock();

  if (!result.success()) {
    return finish(result.code, result.message + L"\n\nNo unverified files were launched.",
                  MB_ICONERROR, options.quiet);
  }
  if (!watcher_started) {
    std::wstring recovery_message;
    if (result.changed_files) {
      recovery_message = safety_restore && safety_restore->success()
                             ? L" Skyrim " + source_version() +
                                   L" was restored as a safety precaution."
                             : L" Automatic runtime restoration also failed.";
    } else {
      recovery_message = L" The installed game files were not modified.";
    }
    if (catalog_cleanup.changed) {
      recovery_message += safety_catalog_restored
                              ? L" ContentCatalog.txt was restored as a safety precaution."
                              : L" ContentCatalog.txt could not be restored.";
    }
    return finish(ExitCode::watcher_start_failed,
                  L"The session watcher could not be started." + recovery_message,
                  MB_ICONERROR, options.quiet);
  }
  if (!catalog_cleanup.success) {
    return finish(ExitCode::content_catalog_cleanup_failed,
                  catalog_cleanup.message +
                      L"\n\nSkyrim will not be started because version " + target_version() +
                      L" may crash with the newer content catalog.",
                  MB_ICONERROR, options.quiet);
  }

  if ((result.changed_files || catalog_cleanup.changed) && !options.quiet &&
      !options.from_skse_loader) {
    MessageBoxW(nullptr,
                (result.message + L"\n\nSKSE will now continue with the matching runtime.").c_str(),
                L"Skyrim Runtime Swapper", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
  }
  return static_cast<int>(ExitCode::success);
}

}  // namespace runtime_swapper::app
