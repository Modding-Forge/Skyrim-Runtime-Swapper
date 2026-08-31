#include "manual_gui.hpp"

#include "content_catalog.hpp"
#include "creation_club.hpp"
#include "diagnostics.hpp"
#include "fixed_runtime.hpp"
#include "runtime_version_reader.hpp"
#include "session.hpp"
#include "storage_operations.hpp"
#include "persistent_dialog.hpp"
#include "unique_handle.hpp"
#include "wine_sidecar.hpp"

#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/session_gate.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>
#include <commctrl.h>

#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace runtime_swapper::app {
namespace {

constexpr int switch_button_id = 1001;
constexpr int restore_button_id = 1002;
constexpr int refresh_button_id = 1003;

struct ManualOperationResult {
  bool success{};
  std::wstring message;
};

class ManualOperationLock {
 public:
  ManualOperationLock() {
    mutex_.reset(CreateMutexW(nullptr, FALSE, operation_mutex_name().c_str()));
    complete_event_.reset(
        CreateEventW(nullptr, TRUE, TRUE, session_complete_event_name().c_str()));
    if (!mutex_ || !complete_event_ ||
        !wait_for_inactive_session_and_lock(complete_event_.get(), mutex_.get(),
                                            5 * 60 * 1000)) {
      return;
    }
    owns_mutex_ = true;
  }

  ~ManualOperationLock() {
    if (owns_mutex_) ReleaseMutex(mutex_.get());
  }

  [[nodiscard]] bool acquired() const noexcept {
    return owns_mutex_;
  }

 private:
  UniqueHandle mutex_;
  UniqueHandle complete_event_;
  bool owns_mutex_{};
};

[[nodiscard]] std::wstring profile_name() {
  return build_profile_label == "boaw" ? L"Best of All Worlds"
                                       : L"Best of Both Worlds";
}

[[nodiscard]] std::filesystem::path resolve_game_root(
    const std::filesystem::path& helper_path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(helper_path, error);
  return error ? helper_path.parent_path() : absolute.parent_path();
}

[[nodiscard]] ManualOperationResult acquire_error() {
  return {false,
          L"The runtime operation lock could not be acquired. Close Skyrim and wait "
          L"for any active restoration to finish."};
}

[[nodiscard]] ManualOperationResult switch_to_fixed_target(
    const std::filesystem::path& game_root) {
  ManualOperationLock lock;
  if (!lock.acquired()) return acquire_error();

  const auto native_probe = is_wine_environment()
                                ? run_wine_sidecar(WineSidecarOperation::probe,
                                                   game_root)
                                : InstallationOperationResult{};
  const auto backend = is_wine_environment()
                           ? native_probe.backend
                           : probe_installation_storage(game_root).backend;
  if (!backend.success()) return {false, backend.message};
  if (backend.mode != SafetyMode::automatic &&
      show_persistent_downgrade_dialog(game_root, backend) !=
          PersistentDialogChoice::accepted) {
    return {false, L"The persistent downgrade was cancelled. No file was changed."};
  }
  UniqueHandle transaction_lock;
  if (!is_wine_environment()) {
    transaction_lock = acquire_transaction_lock(backend.coordination_lock);
    if (!transaction_lock) return acquire_error();
  }
  if (backend.mode == SafetyMode::persistent_with_warning) {
    log_diagnostic(L"Persistent storage risk accepted: riskAccepted=true");
  }
  const auto result = is_wine_environment()
                          ? run_wine_sidecar(
                                WineSidecarOperation::activate_persistent, game_root,
                                backend.mode == SafetyMode::persistent_with_warning)
                          : activate_persistent_target(
                                game_root,
                                backend.mode == SafetyMode::persistent_with_warning);
  log_diagnostic(L"Manual persistent runtime switch: " + result.message);
  return {result.success(), result.message};
}

[[nodiscard]] ManualOperationResult restore_source_runtime(
    const std::filesystem::path& game_root) {
  ManualOperationLock lock;
  if (!lock.acquired()) return acquire_error();
  UniqueHandle transaction_lock;
  if (!is_wine_environment()) {
    const auto probed = probe_installation_storage(game_root);
    if (!probed.success()) return {false, probed.message};
    transaction_lock = acquire_transaction_lock(
        probed.backend.coordination_lock);
    if (!transaction_lock) return acquire_error();
  }

  const auto result = is_wine_environment()
                          ? run_wine_sidecar(
                                WineSidecarOperation::restore_persistent, game_root)
                          : restore_persistent_source(game_root);
  log_diagnostic(L"Manual persistent restore: " + result.message);
  return {result.success(), result.message};
}

[[nodiscard]] std::wstring status_text(const std::filesystem::path& game_root,
                                       FixedRuntimeState fixed_state,
                                       bool fixed_target_verified) {
  std::wstring status = L"Game directory:\n" + game_root.wstring() + L"\n\nProfile: " +
                        profile_name() + L"\nAvailable switch: 1.7.104 <-> " +
                        std::wstring(target_version_label) + L"\n";
  const auto version = read_runtime_version(game_root / L"SkyrimSE.exe");
  status += L"Installed executable: ";
  status += version ? version->to_string() : L"not detected";
  status += L"\nPersistent target: ";
  switch (fixed_state) {
    case FixedRuntimeState::inactive:
      status += L"disabled";
      break;
    case FixedRuntimeState::active:
      status += fixed_target_verified
                    ? L"enabled and verified"
                    : L"enabled, but the managed target files require repair";
      break;
    case FixedRuntimeState::invalid:
      status += L"invalid marker";
      break;
  }
  const auto backend = is_wine_environment()
                           ? run_wine_sidecar(WineSidecarOperation::probe, game_root).backend
                           : probe_installation_storage(game_root).backend;
  status += L"\nStorage mode: " + safety_mode_label(backend.mode);
  if (!backend.vault_path.empty()) {
    status += L"\nRecovery vault: " + backend.vault_path.wstring();
  }
  if (!backend.success()) {
    status += L"\n\nDowngrade unavailable: " + backend.message;
    if (!backend.technical_reason.empty()) {
      status += L"\nTechnical reason: " + backend.technical_reason;
    }
  }
  return status;
}

[[nodiscard]] int show_control_panel(const std::filesystem::path& game_root) {
  for (;;) {
    const auto fixed_state = inspect_fixed_runtime(game_root);
    const bool fixed_target_verified =
        fixed_state == FixedRuntimeState::active &&
        target_runtime_is_active(game_root);
    const auto content =
        status_text(game_root, fixed_state, fixed_target_verified);
    const auto backend = is_wine_environment()
                             ? run_wine_sidecar(WineSidecarOperation::probe,
                                                game_root).backend
                             : probe_installation_storage(game_root).backend;
    const std::wstring switch_label =
        L"Downgrade persistently to Skyrim " + std::wstring(target_version_label);
    const std::wstring restore_label = L"Restore Skyrim 1.7.104";
    std::vector<TASKDIALOG_BUTTON> buttons;
    if (!fixed_target_verified && backend.success() &&
        backend.allows(StorageOperation::activate_persistent)) {
      buttons.push_back({switch_button_id, switch_label.c_str()});
    }
    buttons.push_back({restore_button_id, restore_label.c_str()});
    buttons.push_back({refresh_button_id, L"Refresh"});
    TASKDIALOGCONFIG configuration{};
    configuration.cbSize = sizeof(configuration);
    configuration.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    configuration.pszWindowTitle = L"Skyrim Runtime Swapper";
    configuration.pszMainIcon = TD_INFORMATION_ICON;
    configuration.pszMainInstruction = L"Manual runtime control";
    configuration.pszContent = content.c_str();
    configuration.cButtons = static_cast<UINT>(buttons.size());
    configuration.pButtons = buttons.data();
    configuration.nDefaultButton =
        buttons.front().nButtonID == switch_button_id ? switch_button_id
                                                      : restore_button_id;
    configuration.dwCommonButtons = TDCBF_CLOSE_BUTTON;

    int selected{};
    if (FAILED(TaskDialogIndirect(&configuration, &selected, nullptr, nullptr)) ||
        selected == IDCLOSE || selected == IDCANCEL || selected == 0) {
      return 0;
    }
    if (selected == refresh_button_id) continue;

    const auto result = selected == switch_button_id
                            ? switch_to_fixed_target(game_root)
                            : restore_source_runtime(game_root);
    MessageBoxW(nullptr, result.message.c_str(), L"Skyrim Runtime Swapper",
                MB_OK | (result.success ? MB_ICONINFORMATION : MB_ICONERROR) |
                    MB_SETFOREGROUND);
    if (result.success) return 0;
  }
}

}  // namespace

int run_manual_gui(const std::filesystem::path& helper_path) {
  return show_control_panel(resolve_game_root(helper_path));
}

}  // namespace runtime_swapper::app
