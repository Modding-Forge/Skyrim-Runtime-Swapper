#include "application.hpp"

#include "command_line.hpp"
#include "content_catalog.hpp"
#include "creation_club.hpp"
#include "diagnostics.hpp"
#include "fixed_runtime.hpp"
#include "manual_gui.hpp"
#include "runtime_labels.hpp"
#include "runtime_version_reader.hpp"
#include "session.hpp"
#include "unique_handle.hpp"

#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/exit_code.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/session_gate.hpp>
#include <runtime_swapper/session_plan.hpp>
#include <runtime_swapper/transaction_backend.hpp>

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
      options.restore_content_catalog_after_session,
      options.restore_creation_club_after_session, *options.ready_event_name);
  if (!result.success()) return finish(result.code, result.message, MB_ICONERROR, options.quiet);
  return static_cast<int>(ExitCode::success);
}

}  // namespace

int run(int argc, wchar_t** argv) {
  const auto options = parse_command_line(argc, argv);
  if (argc == 1) return run_manual_gui(options.helper_path);
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

  const auto transaction_lock_path =
      *options.game_root / L".skyrim-runtime-swapper" / L"transaction.lock";
  std::error_code lock_error;
  std::filesystem::create_directories(transaction_lock_path.parent_path(), lock_error);
  UniqueHandle transaction_lock(
      lock_error ? INVALID_HANDLE_VALUE
                 : CreateFileW(transaction_lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_WRITE_THROUGH, nullptr));
  if (!transaction_lock) {
    mutex_lock.unlock();
    return finish(ExitCode::another_instance_failed,
                  L"The durable runtime transaction lock could not be acquired.",
                  MB_ICONERROR, options.quiet);
  }

  const auto backend_probe = transaction_backend().probe(*options.game_root);
  log_diagnostic(L"Backend: " + backend_probe.description);

  const auto fixed_runtime = inspect_fixed_runtime(*options.game_root);
  if (fixed_runtime == FixedRuntimeState::invalid) {
    mutex_lock.unlock();
    return finish(ExitCode::commit_failed,
                  L"The persistent runtime marker is invalid. Open "
                  L"SkyrimRuntimeSwapper.exe and restore Skyrim 1.7.104.",
                  MB_ICONERROR, options.quiet);
  }
  const bool fixed_target = fixed_runtime == FixedRuntimeState::active;
  bool reused_fixed_target = false;
  DowngradeResult recovered;
  if (fixed_target) {
    const auto finalized = finalize_fixed_target_runtime(*options.game_root);
    if (finalized.success()) {
      recovered = finalized;
      reused_fixed_target = true;
    } else if (finalized.code == ExitCode::source_hash_mismatch) {
      recovered = recover_runtime(*options.game_root);
    } else {
      recovered = finalized;
    }
  } else {
    recovered = recover_runtime(*options.game_root);
  }
  log_diagnostic(L"Runtime recovery: " + recovered.message);
  if (!recovered.success()) {
    mutex_lock.unlock();
    return finish(recovered.code,
                  recovered.message +
                      L"\n\nSkyrim was not started. Use Steam's Verify integrity of game files "
                      L"action if recovery remains unavailable.",
                  MB_ICONERROR, options.quiet);
  }
  const auto recovered_catalog = recover_content_catalog(*options.game_root);
  if (!recovered_catalog.success) {
    mutex_lock.unlock();
    return finish(ExitCode::content_catalog_cleanup_failed,
                  recovered_catalog.message + L"\n\nSkyrim was not started.",
                  MB_ICONERROR, options.quiet);
  }
  const auto recovered_creation_club =
      recover_creation_club_content(*options.game_root);
  if (!recovered_creation_club.success) {
    mutex_lock.unlock();
    return finish(ExitCode::creation_club_cleanup_failed,
                  recovered_creation_club.message + L"\n\nSkyrim was not started.",
                  MB_ICONERROR, options.quiet);
  }

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
  auto result = reused_fixed_target
                    ? DowngradeResult{ExitCode::success, false,
                                      L"The verified fixed target runtime is already active."}
                    : downgrade_runtime_after_recovery(*options.game_root, patch_root);
  if (result.success() && fixed_target && !reused_fixed_target) {
    const auto finalized = finalize_fixed_target_runtime(*options.game_root);
    if (!finalized.success()) result = finalized;
  }
  log_diagnostic(L"Runtime prepare: " + result.message);
  ContentCatalogResult catalog_cleanup{true, false, {}};
  CreationClubResult creation_club_cleanup{true, false, {}};
  if (result.success()) {
    catalog_cleanup = remove_incompatible_content_catalog(*options.game_root);
    log_diagnostic(catalog_cleanup.success
                       ? L"ContentCatalog prepare: complete"
                       : L"ContentCatalog prepare failed: " + catalog_cleanup.message);
  }
  if (result.success() && catalog_cleanup.success) {
    creation_club_cleanup =
        quarantine_creation_club_content(*options.game_root);
    log_diagnostic(
        creation_club_cleanup.success
            ? L"Creation Club prepare: complete"
            : L"Creation Club prepare failed: " + creation_club_cleanup.message);
  }

  const auto session_plan = make_session_plan(
      options.from_skse_loader, result.changed_files && !fixed_target,
      catalog_cleanup.changed,
      creation_club_cleanup.changed);
  bool watcher_started = true;
  std::optional<DowngradeResult> safety_restore;
  bool safety_catalog_restored = true;
  bool safety_creation_club_restored = true;
  if (result.success() && catalog_cleanup.success && creation_club_cleanup.success &&
      session_plan.start_watcher) {
    watcher_started =
        options.loader_process_id &&
        launch_session_watcher(options.helper_path, *options.game_root,
                               *options.loader_process_id,
                               session_plan.restore_runtime_after_session,
                               session_plan.restore_content_catalog_after_session,
                               session_plan.restore_creation_club_after_session);
  }
  if (result.success() &&
      (!catalog_cleanup.success || !creation_club_cleanup.success || !watcher_started)) {
    if (result.changed_files && !fixed_target) {
      safety_restore = restore_runtime(*options.game_root);
    }
    if (creation_club_cleanup.changed) {
      safety_creation_club_restored =
          recover_creation_club_content(*options.game_root).success;
    }
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
    if (result.changed_files && !fixed_target) {
      recovery_message = safety_restore && safety_restore->success()
                             ? L" Skyrim " + source_version() +
                                   L" was restored as a safety precaution."
                             : L" Automatic runtime restoration also failed.";
    } else if (fixed_target) {
      recovery_message = L" The fixed target runtime remains active.";
    } else {
      recovery_message = L" The installed game files were not modified.";
    }
    if (catalog_cleanup.changed) {
      recovery_message += safety_catalog_restored
                              ? L" ContentCatalog.txt was restored as a safety precaution."
                              : L" ContentCatalog.txt could not be restored.";
    }
    if (creation_club_cleanup.changed) {
      recovery_message +=
          safety_creation_club_restored
              ? L" Creation Club content was restored as a safety precaution."
              : L" Creation Club content could not be restored.";
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
  if (!creation_club_cleanup.success) {
    return finish(ExitCode::creation_club_cleanup_failed,
                  creation_club_cleanup.message +
                      L"\n\nSkyrim will not be started because Creation Club content "
                      L"could not be quarantined safely.",
                  MB_ICONERROR, options.quiet);
  }

  if ((result.changed_files || catalog_cleanup.changed ||
       creation_club_cleanup.changed) &&
      !options.quiet &&
      !options.from_skse_loader) {
    MessageBoxW(nullptr,
                (result.message + L"\n\nSKSE will now continue with the matching runtime.").c_str(),
                L"Skyrim Runtime Swapper", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
  }
  return static_cast<int>(ExitCode::success);
}

}  // namespace runtime_swapper::app
