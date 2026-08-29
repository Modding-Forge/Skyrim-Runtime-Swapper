#include "manual_gui.hpp"

#include "content_catalog.hpp"
#include "creation_club.hpp"
#include "diagnostics.hpp"
#include "fixed_runtime.hpp"
#include "runtime_version_reader.hpp"
#include "session.hpp"
#include "unique_handle.hpp"

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
  explicit ManualOperationLock(const std::filesystem::path& game_root) {
    mutex_.reset(CreateMutexW(nullptr, FALSE, operation_mutex_name().c_str()));
    complete_event_.reset(
        CreateEventW(nullptr, TRUE, TRUE, session_complete_event_name().c_str()));
    if (!mutex_ || !complete_event_ ||
        !wait_for_inactive_session_and_lock(complete_event_.get(), mutex_.get(),
                                            5 * 60 * 1000)) {
      return;
    }
    owns_mutex_ = true;

    const auto lock_path =
        game_root / L".skyrim-runtime-swapper" / L"transaction.lock";
    std::error_code error;
    std::filesystem::create_directories(lock_path.parent_path(), error);
    if (error) return;
    transaction_lock_.reset(
        CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_WRITE_THROUGH,
                    nullptr));
  }

  ~ManualOperationLock() {
    if (owns_mutex_) ReleaseMutex(mutex_.get());
  }

  [[nodiscard]] bool acquired() const noexcept {
    return owns_mutex_ && static_cast<bool>(transaction_lock_);
  }

 private:
  UniqueHandle mutex_;
  UniqueHandle complete_event_;
  UniqueHandle transaction_lock_;
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
  ManualOperationLock lock(game_root);
  if (!lock.acquired()) return acquire_error();

  const auto backend = transaction_backend().probe(game_root);
  if (!backend.success()) return {false, backend.message};
  const auto fixed_state = inspect_fixed_runtime(game_root);
  if (fixed_state == FixedRuntimeState::invalid) {
    return {false,
            L"The persistent runtime marker is invalid. Restore Skyrim 1.7.104 first."};
  }
  if (fixed_state == FixedRuntimeState::active) {
    const auto finalized = finalize_fixed_target_runtime(game_root);
    if (finalized.success()) return {true, finalized.message};
    if (finalized.code != ExitCode::source_hash_mismatch) {
      return {false, finalized.message};
    }
  }

  const auto recovered_runtime = recover_runtime(game_root);
  if (!recovered_runtime.success()) {
    return {false, recovered_runtime.message};
  }
  const auto recovered_creation_club =
      recover_creation_club_content(game_root);
  if (!recovered_creation_club.success) {
    return {false, recovered_creation_club.message};
  }
  const auto recovered_catalog = recover_content_catalog(game_root);
  if (!recovered_catalog.success) return {false, recovered_catalog.message};

  const auto downgraded = downgrade_runtime_after_recovery(
      game_root, game_root / L"RuntimeSwap" / L"patches");
  if (!downgraded.success()) return {false, downgraded.message};

  const auto marker = enable_fixed_runtime(game_root);
  if (!marker.success) {
    const auto restored = restore_runtime(game_root);
    return {false,
            marker.message +
                (restored.success()
                     ? L" Skyrim 1.7.104 was restored."
                     : L" Automatic runtime restoration also failed.")};
  }
  const auto finalized = finalize_fixed_target_runtime(game_root);
  if (!finalized.success()) {
    const auto marker_removed = disable_fixed_runtime(game_root);
    const auto restored = marker_removed.success ? restore_runtime(game_root)
                                                 : DowngradeResult{};
    return {false,
            finalized.message +
                (restored.success()
                     ? L" Skyrim 1.7.104 was restored."
                     : L" Automatic runtime restoration was incomplete.")};
  }
  log_diagnostic(L"Manual fixed runtime switch: " + finalized.message);
  return {true,
          L"Skyrim " + std::wstring(target_version_label) +
              L" is now fixed as the active runtime.\n\nThe verified 1.7.104 "
              L"fallback backup remains available."};
}

[[nodiscard]] ManualOperationResult restore_source_runtime(
    const std::filesystem::path& game_root) {
  ManualOperationLock lock(game_root);
  if (!lock.acquired()) return acquire_error();

  const auto marker = disable_fixed_runtime(game_root);
  if (!marker.success) return {false, marker.message};

  const auto runtime = restore_runtime(game_root);
  const auto creation_club = recover_creation_club_content(game_root);
  const auto catalog = recover_content_catalog(game_root);
  if (!runtime.success() || !creation_club.success || !catalog.success) {
    std::wstring message;
    if (!runtime.success()) message += runtime.message;
    if (!creation_club.success) {
      if (!message.empty()) message += L"\n";
      message += creation_club.message;
    }
    if (!catalog.success) {
      if (!message.empty()) message += L"\n";
      message += catalog.message;
    }
    return {false, std::move(message)};
  }
  log_diagnostic(L"Manual runtime restore: " + runtime.message);
  return {true, L"Skyrim 1.7.104 is restored and automatic runtime detection is active."};
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
    const std::wstring switch_label =
        L"Switch and keep Skyrim " + std::wstring(target_version_label);
    const std::wstring restore_label = L"Restore Skyrim 1.7.104";
    std::vector<TASKDIALOG_BUTTON> buttons;
    if (!fixed_target_verified) {
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
        fixed_target_verified ? restore_button_id : switch_button_id;
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
