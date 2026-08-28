#include "content_catalog.hpp"

#include "runtime_labels.hpp"

#include <runtime_swapper/file_status.hpp>

#include <windows.h>

#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

namespace runtime_swapper::app {
namespace {

[[nodiscard]] std::optional<std::filesystem::path> content_catalog_path() {
  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (required == 0) return std::nullopt;

  std::vector<wchar_t> local_app_data(required);
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(), required) == 0) {
    return std::nullopt;
  }
  return std::filesystem::path(local_app_data.data()) / L"Skyrim Special Edition" /
         L"ContentCatalog.txt";
}

[[nodiscard]] std::filesystem::path content_catalog_backup(
    const std::filesystem::path& game_root) {
  return game_root / L".skyrim-runtime-swapper" / L"backups" / source_version() /
         L"ContentCatalog.txt";
}

}  // namespace

ContentCatalogResult remove_incompatible_content_catalog(
    const std::filesystem::path& game_root) {
  const auto catalog = content_catalog_path();
  if (!catalog) {
    return {false, false, L"The LOCALAPPDATA environment variable is unavailable."};
  }

  std::error_code error;
  const auto catalog_status = inspect_regular_file(*catalog, error);
  if (catalog_status == RegularFileStatus::missing) return {true, false, {}};
  if (catalog_status != RegularFileStatus::regular) {
    return {false, false, L"ContentCatalog.txt could not be inspected: " + catalog->wstring()};
  }

  const auto backup = content_catalog_backup(game_root);
  std::filesystem::create_directories(backup.parent_path(), error);
  if (error) {
    return {false, false, L"The ContentCatalog.txt backup directory could not be created."};
  }

  auto temporary_backup = backup;
  temporary_backup += L".tmp-" + std::to_wstring(GetCurrentProcessId());
  std::filesystem::remove(temporary_backup, error);
  error.clear();
  if (!std::filesystem::copy_file(*catalog, temporary_backup,
                                  std::filesystem::copy_options::none, error) ||
      error || !MoveFileExW(temporary_backup.c_str(), backup.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temporary_backup, error);
    return {false, false, L"ContentCatalog.txt could not be backed up before removal."};
  }

  if (!std::filesystem::remove(*catalog, error) || error) {
    return {false, false,
            L"The incompatible ContentCatalog.txt could not be removed: " + catalog->wstring()};
  }
  return {true, true, {}};
}

ContentCatalogResult restore_content_catalog(const std::filesystem::path& game_root) {
  const auto catalog = content_catalog_path();
  if (!catalog) {
    return {false, false,
            L"The LOCALAPPDATA environment variable is unavailable for restoration."};
  }

  const auto backup = content_catalog_backup(game_root);
  if (!std::filesystem::is_regular_file(backup)) return {true, false, {}};

  std::error_code error;
  std::filesystem::create_directories(catalog->parent_path(), error);
  if (error || !std::filesystem::copy_file(backup, *catalog,
                                           std::filesystem::copy_options::overwrite_existing,
                                           error)) {
    return {false, false, L"ContentCatalog.txt could not be restored after the game session."};
  }
  return {true, true, {}};
}

}  // namespace runtime_swapper::app
