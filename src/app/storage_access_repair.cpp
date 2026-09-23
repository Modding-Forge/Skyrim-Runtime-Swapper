#include "storage_access_repair.hpp"

#include "path_display.hpp"
#include "content_catalog.hpp"
#include "storage_operations.hpp"
#include "unique_handle.hpp"

#include <runtime_swapper/prepared_storage.hpp>
#include <runtime_swapper/windows_storage_access.hpp>

#include <windows.h>
#include <shellapi.h>

namespace runtime_swapper::app {

bool windows_storage_access_repair_needed(const BackendProbeResult& probe) noexcept {
  return windows_storage_directories_need_repair(probe);
}

StorageAccessRepairResult request_windows_storage_access_repair(
    const std::filesystem::path& helper_path,
    const std::filesystem::path& game_root) noexcept {
  try {
    const std::wstring parameters = L"--repair-storage-access --game-root " +
                                    quote_windows_command_argument(game_root.wstring()) +
                                    L" --quiet";
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.lpVerb = L"runas";
    execute.lpFile = helper_path.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
      return GetLastError() == ERROR_CANCELLED ? StorageAccessRepairResult::cancelled
                                               : StorageAccessRepairResult::failed;
    }
    UniqueHandle process(execute.hProcess);
    if (WaitForSingleObject(process.get(), 60'000) != WAIT_OBJECT_0) {
      return StorageAccessRepairResult::failed;
    }
    DWORD exit_code{};
    if (!GetExitCodeProcess(process.get(), &exit_code) || exit_code != 0) {
      return StorageAccessRepairResult::failed;
    }
    // Recheck as the original, unelevated user. UAC may have used a different
    // administrator account; its successful probe is not sufficient evidence.
    const auto verified = probe_installation_storage(game_root).backend;
    return verified.success() && !windows_storage_directories_need_repair(verified)
               ? StorageAccessRepairResult::succeeded
               : StorageAccessRepairResult::failed;
  } catch (...) { return StorageAccessRepairResult::failed; }
}

StorageAccessRepairResult repair_windows_storage_access(
    const std::filesystem::path& game_root, std::wstring* detail) noexcept {
  try {
    const auto probe = probe_prepared_storage(game_root);
    const auto repaired = repair_windows_storage_directories(probe);
    if (!repaired) {
      if (detail) *detail = repaired.detail;
      return StorageAccessRepairResult::failed;
    }
    const auto catalog = probe_content_catalog_storage(game_root);
    if (!catalog.success() || windows_storage_directories_need_repair(catalog)) {
      const auto catalog_repair = repair_windows_storage_directories(catalog);
      if (!catalog_repair) {
        if (detail) *detail = L"ContentCatalog storage access repair: " +
                              catalog_repair.detail;
        return StorageAccessRepairResult::failed;
      }
    }
    const auto verified = probe_installation_storage(game_root).backend;
    if (!verified.success() || windows_storage_directories_need_repair(verified)) {
      if (detail) *detail = L"Storage access repair verification failed: " +
                            verified.technical_reason + L"; " + verified.message;
      return StorageAccessRepairResult::failed;
    }
    if (detail) *detail = L"SRS storage and recovery-vault ownership and access were repaired and verified.";
    return StorageAccessRepairResult::succeeded;
  } catch (...) {
    if (detail) *detail = L"The SRS storage access repair did not complete.";
    return StorageAccessRepairResult::failed;
  }
}

}  // namespace runtime_swapper::app
