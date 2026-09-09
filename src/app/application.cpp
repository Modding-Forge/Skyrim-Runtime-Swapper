#include "application.hpp"

#include "command_line.hpp"
#include "content_catalog.hpp"
#include "creation_club.hpp"
#include "diagnostics.hpp"
#include "manual_gui.hpp"
#include "path_display.hpp"
#include "persistent_dialog.hpp"
#include "runtime_labels.hpp"
#include "runtime_version_reader.hpp"
#include "session.hpp"
#include "storage_access_repair.hpp"
#include "storage_operations.hpp"
#include "unique_handle.hpp"
#include "wine_sidecar.hpp"

#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/exit_code.hpp>
#include <runtime_swapper/release_version.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/session_gate.hpp>
#include <runtime_swapper/session_plan.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>
#include <commctrl.h>

#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>

namespace runtime_swapper::app {
namespace {

constexpr int repair_storage_access_button_id = 4201;

[[nodiscard]] bool offer_windows_storage_access_repair(
    const CommandLineOptions& options, const BackendProbeResult& probe) {
  if (options.quiet || !options.game_root ||
      !windows_storage_access_repair_needed(probe)) {
    return false;
  }
  const TASKDIALOG_BUTTON buttons[] = {
      {repair_storage_access_button_id, L"Repair SRS folder access"},
      {IDCLOSE, L"Close"},
  };
  const std::wstring content =
      L"An SRS storage, lock, or recovery-vault directory has incorrect ownership or permissions. "
      L"This usually happens after Steam, a mod manager, or SRS was previously run "
      L"as Administrator.\n\n"
      L"Repair changes ownership and private access only for the detected SRS storage "
      L"directories. It does not change Skyrim files. Windows will ask for administrator "
      L"approval.\n\n"
      L"Storage: " + probe.coordination_lock.value.parent_path().parent_path().wstring();
  TASKDIALOGCONFIG configuration{};
  configuration.cbSize = sizeof(configuration);
  configuration.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
  const auto title = application_title();
  configuration.pszWindowTitle = title.c_str();
  configuration.pszMainIcon = TD_WARNING_ICON;
  configuration.pszMainInstruction = L"SRS folder access needs repair";
  configuration.pszContent = content.c_str();
  configuration.cButtons = static_cast<UINT>(std::size(buttons));
  configuration.pButtons = buttons;
  configuration.nDefaultButton = IDCLOSE;
  int selected{};
  if (FAILED(TaskDialogIndirect(&configuration, &selected, nullptr, nullptr)) ||
      selected != repair_storage_access_button_id) {
    return false;
  }
  const auto repaired = request_windows_storage_access_repair(
      options.helper_path, *options.game_root);
  log_diagnostic(L"Windows storage-access repair: result=" +
                 std::to_wstring(static_cast<int>(repaired)));
  if (repaired == StorageAccessRepairResult::succeeded) return true;
  const wchar_t* message = repaired == StorageAccessRepairResult::cancelled
                               ? L"The SRS folder access repair was cancelled."
                               : L"Windows could not repair the SRS folder access.";
  MessageBoxW(nullptr, message, title.c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
  return false;
}

[[nodiscard]] bool offer_linux_storage_access_repair(
    const CommandLineOptions& options, const BackendProbeResult& probe) {
  if (options.quiet || !options.game_root ||
      probe.technical_reason != L"native-storage-ownership") {
    return false;
  }
  const TASKDIALOG_BUTTON buttons[] = {
      {repair_storage_access_button_id, L"Repair SRS folder access"},
      {IDCLOSE, L"Close"},
  };
  const std::wstring content =
      L"The native Linux SRS storage directory belongs to a different user. "
      L"This usually happens after Steam, a mod manager, or SRS was previously "
      L"started with sudo or as root.\n\n"
      L"Repair changes ownership only inside the detected SRS storage directory. "
      L"It does not change Skyrim files. Your Linux desktop may ask for administrator "
      L"authentication.";
  TASKDIALOGCONFIG configuration{};
  configuration.cbSize = sizeof(configuration);
  configuration.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
  const auto title = application_title();
  configuration.pszWindowTitle = title.c_str();
  configuration.pszMainIcon = TD_WARNING_ICON;
  configuration.pszMainInstruction = L"SRS folder access needs repair";
  configuration.pszContent = content.c_str();
  configuration.cButtons = static_cast<UINT>(std::size(buttons));
  configuration.pButtons = buttons;
  configuration.nDefaultButton = IDCLOSE;
  int selected{};
  if (FAILED(TaskDialogIndirect(&configuration, &selected, nullptr, nullptr)) ||
      selected != repair_storage_access_button_id) {
    return false;
  }
  const auto repaired = run_wine_sidecar(
      WineSidecarOperation::repair_storage_access, *options.game_root);
  log_operation_result(L"linux-storage-access-repair", repaired);
  if (repaired.success()) return true;
  MessageBoxW(nullptr, repaired.message.c_str(), title.c_str(),
              MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
  return false;
}

[[nodiscard]] std::wstring wide_ascii(std::string_view value) {
  return std::wstring(value.begin(), value.end());
}

[[nodiscard]] std::wstring operation_label(int argc, const CommandLineOptions& options) {
  if (argc == 1) return L"manual-gui";
  if (options.watch) return L"session-watcher";
  return options.from_skse_loader ? L"skse-launch" : L"runtime-launch";
}

[[nodiscard]] std::wstring environment_label(bool wine) {
  if (!wine) return L"Windows";
  return GetEnvironmentVariableW(L"STEAM_COMPAT_DATA_PATH", nullptr, 0) != 0 ||
                 GetEnvironmentVariableW(L"SteamGameId", nullptr, 0) != 0
             ? L"Proton (Windows process with native Linux sidecar)"
             : L"Wine (Windows process with native Linux sidecar)";
}

void log_session_header(int argc, const CommandLineOptions& options, bool wine) {
  std::wstring header =
      L"Session: SRS=" + wide_ascii(release_version_utf8) + L"; log-format=2; profile=" +
      wide_ascii(build_profile_label) + L"; operation=" + operation_label(argc, options) +
      L"; runtime=" + std::wstring(source_version_label) + L" -> " +
      std::wstring(target_version_label) + L"; environment=" + environment_label(wine);
  if (options.game_root) {
    header += L"; game-root=" + display_path(*options.game_root);
  } else {
    header += L"; helper=" + display_path(options.helper_path);
  }
  log_diagnostic_session(header, !options.watch);
}

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
  if (!options.loader_process_id || !options.ready_event_name || !options.diagnostic_session_id ||
      !options.diagnostic_parent_run_id) {
    return finish(ExitCode::invalid_arguments,
                  L"The watcher did not receive valid session information.", MB_ICONERROR,
                  options.quiet);
  }
  const auto result = watch_session_and_restore(
      *options.game_root, *options.loader_process_id, options.restore_runtime_after_session,
      options.restore_content_catalog_after_session, options.restore_creation_club_after_session,
      *options.ready_event_name);
  if (!result.success()) {
    return finish(result.code, result.message, MB_ICONERROR, options.quiet);
  }
  return static_cast<int>(ExitCode::success);
}

[[nodiscard]] bool restore_after_watcher_failure(const std::filesystem::path& game_root,
                                                 const InstallationOperationResult& prepared) {
  if (is_wine_environment()) {
    return run_wine_sidecar(WineSidecarOperation::recover, game_root).success();
  }
  bool success = true;
  if (!prepared.persistent && prepared.runtime_changed) {
    success = restore_runtime(game_root).success() && success;
  }
  if (!prepared.persistent && prepared.creation_club_changed) {
    success = recover_creation_club_content(game_root).success && success;
  }
  if (prepared.content_catalog_changed && !prepared.content_catalog_persistent) {
    success = recover_content_catalog(game_root).success && success;
  }
  return success;
}

} // namespace

int run(int argc, wchar_t** argv) {
  const auto options = parse_command_line(argc, argv);
  initialize_diagnostic_run(options.diagnostic_session_id, options.diagnostic_parent_run_id);
  const bool wine = is_wine_environment();
  log_session_header(argc, options, wine);
  if (argc == 1) return run_manual_gui(options.helper_path);
  if (!options.game_root) {
    return finish(ExitCode::invalid_arguments, L"The game directory was not specified.",
                  MB_ICONERROR, options.quiet);
  }
  if (options.repair_storage_access) {
    std::wstring detail;
    const auto repaired = repair_windows_storage_access(*options.game_root, &detail);
    log_diagnostic(L"Elevated Windows storage-access repair: " + detail);
    return repaired == StorageAccessRepairResult::succeeded
               ? static_cast<int>(ExitCode::success)
               : static_cast<int>(ExitCode::internal_error);
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
                  L"The previous Skyrim session was not restored before the timeout.", MB_ICONERROR,
                  options.quiet);
  }
  MutexLock mutex_lock(mutex.get());

  UniqueHandle transaction_lock;

  const auto executable = *options.game_root / L"SkyrimSE.exe";
  if (!std::filesystem::is_regular_file(executable)) {
    mutex_lock.unlock();
    return finish(ExitCode::game_not_found, L"SkyrimSE.exe was not found in the game directory.",
                  MB_ICONERROR, options.quiet);
  }
  const auto version = read_runtime_version(executable);
  if (!version) {
    mutex_lock.unlock();
    return finish(ExitCode::version_read_failed, L"The installed Skyrim version could not be read.",
                  MB_ICONERROR, options.quiet);
  }
  if (*version != source_runtime && *version != target_runtime) {
    mutex_lock.unlock();
    return finish(ExitCode::unsupported_runtime,
                  L"Unsupported Skyrim version: " + version->to_string() +
                      L"\n\nSRS supports only the exact Steam source or package target files. "
                      L"Modified, unofficial, or unlicensed game files, including pirated copies, "
                      L"cannot be made compatible." +
                      L"\n\nNo files were changed.",
                  MB_ICONERROR, options.quiet);
  }

  InstallationOperationResult prepared;
  auto probe = BackendProbeResult{};
  bool risk_accepted = false;

  if (wine) {
    prepared = run_wine_sidecar(WineSidecarOperation::prepare_launch, *options.game_root);
    probe = prepared.backend;
    if (prepared.code == ExitCode::user_cancelled && probe.success()) {
      if (options.quiet || show_persistent_downgrade_dialog(*options.game_root, probe) !=
                               PersistentDialogChoice::accepted) {
        mutex_lock.unlock();
        log_diagnostic(options.quiet ? L"Persistent downgrade requires interactive consent; launch "
                                       L"cancelled in quiet mode."
                                     : L"Persistent downgrade cancelled by the user.");
        return static_cast<int>(ExitCode::user_cancelled);
      }
      risk_accepted = probe.mode == SafetyMode::persistent_with_warning;
      if (risk_accepted) {
        log_diagnostic(L"Persistent storage risk accepted: riskAccepted=true");
      }
      prepared = run_wine_sidecar(WineSidecarOperation::prepare_launch, *options.game_root,
                                  risk_accepted, true);
      probe = prepared.backend;
    }
  } else {
    probe = probe_installation_storage(*options.game_root).backend;
  }
  if (wine && !prepared.success() && offer_linux_storage_access_repair(options, probe)) {
    prepared = run_wine_sidecar(WineSidecarOperation::prepare_launch, *options.game_root);
    probe = prepared.backend;
    if (prepared.code == ExitCode::user_cancelled && probe.success()) {
      if (options.quiet || show_persistent_downgrade_dialog(*options.game_root, probe) !=
                               PersistentDialogChoice::accepted) {
        mutex_lock.unlock();
        return static_cast<int>(ExitCode::user_cancelled);
      }
      risk_accepted = probe.mode == SafetyMode::persistent_with_warning;
      prepared = run_wine_sidecar(WineSidecarOperation::prepare_launch, *options.game_root,
                                  risk_accepted, true);
      probe = prepared.backend;
    }
  }
  log_storage_probe(probe);
  if (!probe.success()) {
    if (!wine && offer_windows_storage_access_repair(options, probe)) {
      probe = probe_installation_storage(*options.game_root).backend;
      log_storage_probe(probe);
    }
    if (!probe.success()) {
      mutex_lock.unlock();
      if (!options.quiet) show_hard_blocked_dialog(probe);
      return finish(probe.code, probe.message + L"\n\nNo files were changed.", MB_ICONERROR, true);
    }
  }

  if (!wine) {
    const auto persistent_state =
        inspect_persistent_runtime(*options.game_root, nullptr, nullptr, false);
    if (persistent_state == PersistentRuntimeState::invalid) {
      mutex_lock.unlock();
      return finish(ExitCode::journal_corrupt,
                    L"The persistent recovery markers are inconsistent. Skyrim was not "
                    L"started.",
                    MB_ICONERROR, options.quiet);
    }
    if (probe.mode != SafetyMode::automatic &&
        persistent_state == PersistentRuntimeState::inactive) {
      if (options.quiet || show_persistent_downgrade_dialog(*options.game_root, probe) !=
                               PersistentDialogChoice::accepted) {
        mutex_lock.unlock();
        return static_cast<int>(ExitCode::user_cancelled);
      }
      risk_accepted = probe.mode == SafetyMode::persistent_with_warning;
    }
    transaction_lock = acquire_transaction_lock(probe.coordination_lock);
    if (!transaction_lock) {
      if (offer_windows_storage_access_repair(options, probe)) {
        probe = probe_installation_storage(*options.game_root).backend;
        if (probe.success()) transaction_lock = acquire_transaction_lock(probe.coordination_lock);
      }
    }
    if (!transaction_lock) {
      mutex_lock.unlock();
      return finish(ExitCode::another_instance_failed,
                    L"The durable runtime transaction lock could not be acquired.", MB_ICONERROR,
                    options.quiet);
    }
    // The UI probe above exists only to obtain consent and the external lock
    // path. Recovery, the final probe, and activation share one prepared core
    // operation so authenticated large files and native handles are reused.
    prepared = prepare_launch(*options.game_root, true, risk_accepted);
  }
  log_operation_result(L"prepare-launch", prepared);
  if (prepared.success() && prepared.message != probe.message) {
    log_diagnostic(L"Installation prepare: " + prepared.message);
  }
  if (!prepared.success()) {
    mutex_lock.unlock();
    return finish(prepared.code,
                  prepared.message + L"\n\nSkyrim was not started. No unverified state "
                                     L"was launched.",
                  MB_ICONERROR, options.quiet);
  }

  const bool restore_runtime_after_session = !prepared.persistent && prepared.runtime_changed;
  const bool restore_creation_club_after_session =
      !prepared.persistent && prepared.creation_club_changed;
  const bool restore_content_catalog_after_session =
      prepared.content_catalog_changed && !prepared.content_catalog_persistent;
  const auto session_plan =
      make_session_plan(options.from_skse_loader, restore_runtime_after_session,
                        restore_content_catalog_after_session, restore_creation_club_after_session);

  bool watcher_started = true;
  if (session_plan.start_watcher) {
    watcher_started =
        options.loader_process_id &&
        launch_session_watcher(options.helper_path, *options.game_root, *options.loader_process_id,
                               session_plan.restore_runtime_after_session,
                               session_plan.restore_content_catalog_after_session,
                               session_plan.restore_creation_club_after_session,
                               diagnostic_session_id(), diagnostic_run_id());
  }
  if (!watcher_started) {
    const bool restored = restore_after_watcher_failure(*options.game_root, prepared);
    mutex_lock.unlock();
    return finish(ExitCode::watcher_start_failed,
                  L"The session watcher could not be started." +
                      std::wstring(restored ? L" All session-scoped changes were restored."
                                            : L" Recovery remains pending and Skyrim was not "
                                              L"started."),
                  MB_ICONERROR, options.quiet);
  }
  mutex_lock.unlock();

  if (prepared.changed && !options.quiet && !options.from_skse_loader) {
    MessageBoxW(
        nullptr,
        (prepared.message + L"\n\nSKSE will now continue with the matching runtime.").c_str(),
        application_title().c_str(), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
  }
  return static_cast<int>(ExitCode::success);
}

} // namespace runtime_swapper::app
