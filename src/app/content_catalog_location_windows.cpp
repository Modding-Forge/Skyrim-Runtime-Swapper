#include "content_catalog_location.hpp"

#include <windows.h>

#include <filesystem>
#include <vector>

namespace runtime_swapper::app {

std::optional<std::filesystem::path> resolve_content_catalog_path() {
  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (required == 0) return std::nullopt;
  std::vector<wchar_t> local_app_data(required);
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(), required) ==
      0) {
    return std::nullopt;
  }
  return std::filesystem::path(local_app_data.data()) /
         L"Skyrim Special Edition" / L"ContentCatalog.txt";
}

}  // namespace runtime_swapper::app
