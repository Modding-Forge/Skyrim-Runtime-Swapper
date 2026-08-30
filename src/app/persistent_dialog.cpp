#include "persistent_dialog.hpp"

#include <runtime_swapper/runtime_version.hpp>

#include <windows.h>
#include <commctrl.h>

#include <iterator>
#include <string>

namespace runtime_swapper::app {
namespace {

constexpr int persistent_button_id = 4101;

}  // namespace

PersistentDialogChoice show_persistent_downgrade_dialog(
    const std::filesystem::path& game_root, const BackendProbeResult& probe) {
  std::wstring content =
      L"Automatic restoration is not considered safe on this drive. Runtime Swapper can "
      L"keep Skyrim " +
      std::wstring(target_version_label) +
      L" active and retain verified original files in:\n\n" +
      probe.vault_path.wstring() + L"\n\nGame volume: " +
      probe.target_volume.description + L"\nRecovery vault: " +
      probe.vault_volume.description + L"\n\nTo restore Skyrim " +
      std::wstring(source_version_label) + L" later, run:\n\n" +
      (game_root / L"SkyrimRuntimeSwapper.exe").wstring() +
      L"\n\nand select Restore Skyrim " + std::wstring(source_version_label) +
      L".\n\nDo not delete or modify the recovery vault while the persistent "
      L"downgrade is active.";
  if (probe.mode == SafetyMode::persistent_with_warning) {
    content +=
        L"\n\nThe filesystem could not be fully classified. Recovery files are verified, "
        L"but a power loss may leave the game directory requiring manual recovery.";
  }
  const std::wstring button =
      probe.mode == SafetyMode::persistent_with_warning
          ? L"Downgrade persistently anyway"
          : L"Downgrade persistently";
  const TASKDIALOG_BUTTON buttons[] = {
      {persistent_button_id, button.c_str()},
  };
  TASKDIALOGCONFIG configuration{};
  configuration.cbSize = sizeof(configuration);
  configuration.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
  configuration.pszWindowTitle = L"Skyrim Runtime Swapper";
  configuration.pszMainIcon = TD_WARNING_ICON;
  configuration.pszMainInstruction = L"Persistent downgrade required";
  configuration.pszContent = content.c_str();
  configuration.cButtons = static_cast<UINT>(std::size(buttons));
  configuration.pButtons = buttons;
  configuration.nDefaultButton = persistent_button_id;
  configuration.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  int selected{};
  if (FAILED(TaskDialogIndirect(&configuration, &selected, nullptr, nullptr))) {
    return PersistentDialogChoice::cancelled;
  }
  return selected == persistent_button_id ? PersistentDialogChoice::accepted
                                          : PersistentDialogChoice::cancelled;
}

void show_hard_blocked_dialog(const BackendProbeResult& probe) {
  const std::wstring content = probe.message + L"\n\nTechnical reason: " +
                               probe.technical_reason +
                               L"\n\nNo game file was changed.";
  TASKDIALOGCONFIG configuration{};
  configuration.cbSize = sizeof(configuration);
  configuration.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
  configuration.pszWindowTitle = L"Skyrim Runtime Swapper";
  configuration.pszMainIcon = TD_ERROR_ICON;
  configuration.pszMainInstruction = L"Downgrade is not recoverable";
  configuration.pszContent = content.c_str();
  configuration.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  int selected{};
  (void)TaskDialogIndirect(&configuration, &selected, nullptr, nullptr);
}

}  // namespace runtime_swapper::app
